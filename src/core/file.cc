/*
 * This file is open source software, licensed to you under the terms
 * of the Apache License, Version 2.0 (the "License").  See the NOTICE file
 * distributed with this work for additional information regarding copyright
 * ownership.  You may not use this file except in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
/*
 * Copyright 2019 ScyllaDB
 */


#include <algorithm>
#include <atomic>
#include <coroutine>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <vector>
#include <seastar/util/assert.hh>

#define __user /* empty */  // for xfs includes, below

// Define STATX_DIOALIGN and related fields if not available from system headers (Linux < 6.1)
#ifndef STATX_DIOALIGN
#define STATX_DIOALIGN 0x00002000U
// Check if struct statx has the stx_dio_mem_align field (added in Linux 6.1)
// We need to extend the structure for older kernels/headers
#define SEASTAR_STATX_NEEDS_DIO_FIELDS 1
#endif

#include <sys/syscall.h>
#include <dirent.h>
#include <linux/types.h> // for xfs, below
#include <linux/fs.h> // BLKBSZGET
#include <linux/major.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <fcntl.h>
#include <xfs/linux.h>
/*
 * With package xfsprogs-devel >= 5.14.1, `fallthrough` has defined to
 * fix compilation warning in header <xfs/linux.h>,
 * (see: https://git.kernel.org/pub/scm/fs/xfs/xfsprogs-dev.git/commit/?id=df9c7d8d8f3ed0785ed83e7fd0c7ddc92cbfbe15)
 * There is a confliction with c++ keyword `fallthrough`, so undefine fallthrough here.
 */
#undef fallthrough
#define min min    /* prevent xfs.h from defining min() as a macro */
#include <xfs/xfs.h>
#undef min

#include <seastar/core/align.hh>
#include <seastar/core/internal/uname.hh>
#include <seastar/core/internal/io_intent.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/file.hh>
#include <seastar/core/report_exception.hh>
#include <seastar/util/later.hh>
#include <seastar/util/defer.hh>
#include <seastar/util/internal/magic.hh>
#include <seastar/util/internal/iovec_utils.hh>
#include <seastar/core/io_queue.hh>
#include <seastar/core/queue.hh>
#include <seastar/coroutine/as_future.hh>
#include "core/file-impl.hh"
#include "core/syscall_result.hh"
#include "core/thread_pool.hh"

namespace seastar {

namespace internal {

struct alignments {
    unsigned memory;
    unsigned disk_read;
    unsigned disk_write;
    unsigned disk_overwrite;
};

// Minimum memory alignment required by posix_memalign
// Must be power of 2 and multiple of sizeof(void*) (8 bytes on 64-bit, 4 on 32-bit)
// Since Linux 6.1, O_DIRECT buffer addresses can use DMA alignment (as low as 4 bytes for NVMe)
// instead of logical block size (typically 512 bytes), but we must satisfy posix_memalign's
// requirement which is stricter on 64-bit systems (sizeof(void*) = 8 > 4)
static constexpr unsigned min_memory_alignment = sizeof(void*);

struct fs_info {
    uint32_t block_size;
    bool append_challenged;
    unsigned append_concurrency;
    bool fsync_is_exclusive;
    bool nowait_works;
    std::optional<alignments> align;
};

};

using namespace internal::linux_abi;

file_handle::file_handle(const file_handle& x)
        : _impl(x._impl ? x._impl->clone() : std::unique_ptr<file_handle_impl>()) {
}

file_handle::file_handle(file_handle&& x) noexcept = default;

file_handle&
file_handle::operator=(const file_handle& x) {
    return operator=(file_handle(x));
}

file_handle&
file_handle::operator=(file_handle&&) noexcept = default;

file
file_handle::to_file() const & {
    return file_handle(*this).to_file();
}

file
file_handle::to_file() && {
    return file(std::move(*_impl).to_file());
}

posix_file_impl::posix_file_impl(int fd, open_flags f, file_open_options options, dev_t device_id, const internal::fs_info& fsi)
        : _nowait_works(fsi.nowait_works)
        , _skip_flush(options.skip_flush)
        , _aio_fdatasync(engine().have_aio_fdatasync())
        , _device_id(device_id)
        , _io_queue(engine().get_io_queue(_device_id))
        , _open_flags(f)
        , _fd(fd)
{
    if (fsi.align.has_value()) {
        _memory_dma_alignment = fsi.align->memory;
        _disk_read_dma_alignment = fsi.align->disk_read;
        _disk_write_dma_alignment = fsi.align->disk_write;
        _disk_overwrite_dma_alignment = fsi.align->disk_overwrite;
    }
    configure_io_lengths();
}

posix_file_impl::~posix_file_impl() {
    if (_refcount && _refcount->fetch_add(-1, std::memory_order_relaxed) != 1) {
        return;
    }
    delete _refcount;
    if (_fd != -1) {
        // Note: close() can be a blocking operation on NFS
        ::close(_fd);
    }
}

void posix_file_impl::configure_io_lengths() noexcept {
    auto limits = _io_queue.get_request_limits();
    _read_max_length = std::min<size_t>(_read_max_length, limits.max_read);
    _write_max_length = std::min<size_t>(_write_max_length, limits.max_write);
}

std::unique_ptr<seastar::file_handle_impl>
posix_file_impl::dup() {
    if (!_refcount) {
        _refcount = new std::atomic<unsigned>(1u);
    }
    auto ret = std::make_unique<posix_file_handle_impl>(_fd, _open_flags, _refcount, _device_id,
            _memory_dma_alignment, _disk_read_dma_alignment, _disk_write_dma_alignment, _disk_overwrite_dma_alignment,
            _nowait_works, _skip_flush, _aio_fdatasync);
    _refcount->fetch_add(1, std::memory_order_relaxed);
    return ret;
}

posix_file_impl::posix_file_impl(int fd, open_flags f, std::atomic<unsigned>* refcount, dev_t device_id,
        uint32_t memory_dma_alignment,
        uint32_t disk_read_dma_alignment,
        uint32_t disk_write_dma_alignment,
        uint32_t disk_overwrite_dma_alignment,
        bool nowait_works, bool skip_flush, bool aio_fdatasync)
        : _refcount(refcount)
        , _nowait_works(nowait_works)
        , _skip_flush(skip_flush)
        , _aio_fdatasync(aio_fdatasync)
        , _device_id(device_id)
        , _io_queue(engine().get_io_queue(_device_id))
        , _open_flags(f)
        , _fd(fd) {
    _memory_dma_alignment = memory_dma_alignment;
    _disk_read_dma_alignment = disk_read_dma_alignment;
    _disk_write_dma_alignment = disk_write_dma_alignment;
    _disk_overwrite_dma_alignment = disk_overwrite_dma_alignment;
    configure_io_lengths();
}

future<>
posix_file_impl::flush() noexcept {
    if (_skip_flush || ((_open_flags & open_flags::dsync) != open_flags{})) {
        // If the file is opened with open_flags::dsync, all writes are already
        // fdatasync-ed to the disk, so we don't need to fdatasync again.
        return make_ready_future<>();
    }

    reactor::io_stats::local().fsyncs++;
    return fdatasync(_aio_fdatasync, _fd, _io_queue.sink());
}

future<>
reactor::fdatasync(int fd) noexcept {
    if (_cfg.bypass_fsync) {
        return make_ready_future<>();
    }
    _io_stats.fsyncs++;
    return posix_file_impl::fdatasync(_cfg.have_aio_fsync, fd, _io_sink);
}

future<> posix_file_impl::fdatasync(bool with_aio, int fd, internal::io_sink& sink) {
    if (with_aio) {
        // Does not go through the I/O queue, but has to be deleted
        struct fsync_io_desc final : public io_completion {
            promise<> _pr;
        public:
            virtual void complete(size_t res) noexcept override {
                _pr.set_value();
                delete this;
            }

            virtual void set_exception(std::exception_ptr eptr) noexcept override {
                _pr.set_exception(std::move(eptr));
                delete this;
            }

            future<> get_future() {
                return _pr.get_future();
            }
        };

        auto desc = new fsync_io_desc;
        auto fut = desc->get_future();
        auto req = internal::io_request::make_fdatasync(fd);
        sink.submit(desc, std::move(req));
        co_await std::move(fut);
        co_return;
    }
    syscall_result<int> sr = co_await engine()._thread_pool->submit<syscall_result<int>>(
            internal::thread_pool_submit_reason::file_operation, [fd] {
        return wrap_syscall<int>(::fdatasync(fd));
    });
    sr.throw_if_error();
}

future<struct stat>
posix_file_impl::stat() noexcept {
    auto ret = co_await engine()._thread_pool->submit<syscall_result_extra<struct stat>>(
            internal::thread_pool_submit_reason::file_operation, [fd = _fd] {
        struct stat st;
        auto ret = ::fstat(fd, &st);
        return wrap_syscall(ret, st);
    });
    ret.throw_if_error();
    co_return ret.extra;
}

future<struct stat>
posix_file_impl::statat(std::string_view name, int flags) noexcept {
    auto ret = co_await engine()._thread_pool->submit<syscall_result_extra<struct stat>>(
            internal::thread_pool_submit_reason::file_operation, [fd = _fd, name = sstring(name), flags] {
        struct stat st;
        auto ret = ::fstatat(fd, name.data(), &st, flags);
        return wrap_syscall(ret, st);
    });
    ret.throw_if_error();
    co_return ret.extra;
}

future<>
posix_file_impl::truncate(uint64_t length) noexcept {
    auto sr = co_await engine()._thread_pool->submit<syscall_result<int>>(
            internal::thread_pool_submit_reason::file_operation, [this, length] {
        return wrap_syscall<int>(::ftruncate(_fd, length));
    });
    sr.throw_if_error();
}

future<int>
posix_file_impl::ioctl(uint64_t cmd, void* argp) noexcept {
    auto sr = co_await engine()._thread_pool->submit<syscall_result<int>>(
            internal::thread_pool_submit_reason::file_operation, [this, cmd, argp] () mutable {
        return wrap_syscall<int>(::ioctl(_fd, cmd, argp));
    });
    sr.throw_if_error();
    // Some ioctls require to return a positive integer back.
    co_return sr.result;
}

future<int>
posix_file_impl::ioctl_short(uint64_t cmd, void* argp) noexcept {
    int ret = ::ioctl(_fd, cmd, argp);
    if (ret == -1) {
        return make_exception_future<int>(
                std::system_error(errno, std::system_category(), "ioctl failed"));
    }
    return make_ready_future<int>(ret);
}

future<int>
posix_file_impl::fcntl(int op, uintptr_t arg) noexcept {
    auto sr = co_await engine()._thread_pool->submit<syscall_result<int>>(
            internal::thread_pool_submit_reason::file_operation, [this, op, arg] () mutable {
        return wrap_syscall<int>(::fcntl(_fd, op, arg));
    });
    sr.throw_if_error();
    // Some fcntls require to return a positive integer back.
    co_return sr.result;
}

future<int>
posix_file_impl::fcntl_short(int op, uintptr_t arg) noexcept {
    int ret = ::fcntl(_fd, op, arg);
    if (ret == -1) {
        return make_exception_future<int>(
                std::system_error(errno, std::system_category(), "fcntl failed"));
    }
    return make_ready_future<int>(ret);
}

future<>
posix_file_impl::discard(uint64_t offset, uint64_t length) noexcept {
    auto sr = co_await engine()._thread_pool->submit<syscall_result<int>>(
            internal::thread_pool_submit_reason::file_operation, [this, offset, length] () mutable {
        return wrap_syscall<int>(::fallocate(_fd, FALLOC_FL_PUNCH_HOLE|FALLOC_FL_KEEP_SIZE,
            offset, length));
    });
    sr.throw_if_error();
}

future<>
posix_file_impl::allocate(uint64_t position, uint64_t length) noexcept {
#ifdef FALLOC_FL_ZERO_RANGE
    // FALLOC_FL_ZERO_RANGE is fairly new, so don't fail if it's not supported.
    static bool supported = true;
    if (!supported) {
        co_return;
    }
    auto sr = co_await engine()._thread_pool->submit<syscall_result<int>>(
            internal::thread_pool_submit_reason::file_operation, [this, position, length] () mutable {
        auto ret = ::fallocate(_fd, FALLOC_FL_ZERO_RANGE|FALLOC_FL_KEEP_SIZE, position, length);
        if (ret == -1 && errno == EOPNOTSUPP) {
            ret = 0;
            supported = false; // Racy, but harmless.  At most we issue an extra call or two.
        }
        return wrap_syscall<int>(ret);
    });
    sr.throw_if_error();
#else
    return make_ready_future<>();
#endif
}

future<uint64_t>
posix_file_impl::size() noexcept {
    auto r = ::lseek(_fd, 0, SEEK_END);
    if (r == -1) {
        return make_exception_future<uint64_t>(std::system_error(errno, std::system_category()));
    }
    return make_ready_future<uint64_t>(r);
}

future<>
posix_file_impl::close() noexcept {
    if (_fd == -1) {
        seastar_logger.warn("double close() detected, contact support");
        return make_ready_future<>();
    }
    auto fd = _fd;
    _fd = -1;  // Prevent a concurrent close (which is illegal) from closing another file's fd
    if (_refcount && _refcount->fetch_add(-1, std::memory_order_relaxed) != 1) {
        _refcount = nullptr;
        return make_ready_future<>();
    }
    delete _refcount;
    _refcount = nullptr;
    auto closed = make_ready_future<syscall_result<int>>(0, 0);
    if ((_open_flags & open_flags::ro) != open_flags{}) {
        closed = futurize_invoke([fd] () noexcept {
            return wrap_syscall<int>(::close(fd));
        });
    } else {
        closed = std::invoke([fd] () noexcept {
            try {
                return engine()._thread_pool->submit<syscall_result<int>>(
                        internal::thread_pool_submit_reason::file_operation, [fd] {
                    return wrap_syscall<int>(::close(fd));
                });
            } catch (...) {
                report_exception("Running ::close() in reactor thread, submission failed with exception", std::current_exception());
                return make_ready_future<syscall_result<int>>(wrap_syscall<int>(::close(fd)));
            }
        });
    }
    return closed.then([] (syscall_result<int> sr) {
      try {
        sr.throw_if_error();
      } catch (...) {
        report_exception("close() syscall failed", std::current_exception());
      }
    });
}

future<uint64_t>
blockdev_file_impl::size() noexcept {
    auto ret = co_await engine()._thread_pool->submit<syscall_result_extra<size_t>>(
            internal::thread_pool_submit_reason::file_operation, [this] {
        uint64_t size;
        int ret = ::ioctl(_fd, BLKGETSIZE64, &size);
        return wrap_syscall(ret, size);
    });
    ret.throw_if_error();
    co_return ret.extra;
}

static std::optional<directory_entry_type> dirent_type(const linux_dirent64& de) {
    std::optional<directory_entry_type> type;
    switch (de.d_type) {
    case DT_BLK:
        type = directory_entry_type::block_device;
        break;
    case DT_CHR:
        type = directory_entry_type::char_device;
        break;
    case DT_DIR:
        type = directory_entry_type::directory;
        break;
    case DT_FIFO:
        type = directory_entry_type::fifo;
        break;
    case DT_REG:
        type = directory_entry_type::regular;
        break;
    case DT_LNK:
        type = directory_entry_type::link;
        break;
    case DT_SOCK:
        type = directory_entry_type::socket;
        break;
    default:
        // unknown, ignore
        ;
    }
    return type;
}

future<size_t> posix_file_impl::read_directory(int fd, char* buffer, size_t buffer_size) {
    syscall_result<long> ret = co_await engine()._thread_pool->submit<syscall_result<long>>(
            internal::thread_pool_submit_reason::file_operation, [fd, buffer, buffer_size] () {
        auto ret = ::syscall(__NR_getdents64, fd, reinterpret_cast<linux_dirent64*>(buffer), buffer_size);
        return wrap_syscall(ret);
    });
    ret.throw_if_error();
    co_return ret.result;
}

future<size_t> reactor::read_directory(int fd, char* buffer, size_t buffer_size) {
    return posix_file_impl::read_directory(fd, buffer, buffer_size);
}

list_directory_generator_type make_list_directory_generator(int fd) {
    temporary_buffer<char> buf(8192);

    while (true) {
        auto size = co_await posix_file_impl::read_directory(fd, buf.get_write(), buf.size());
        if (size == 0) {
            co_return;
        }

        for (const char* b = buf.get(); b < buf.get() + size; ) {
            const auto de = reinterpret_cast<const linux_dirent64*>(b);
            b += de->d_reclen;
            sstring name(de->d_name);
            if (name == "." || name == "..") {
                continue;
            }
            std::optional<directory_entry_type> type = dirent_type(*de);
            directory_entry ret{std::move(name), type};
            co_yield ret;
        }
    }
}

list_directory_generator_type posix_file_impl::experimental_list_directory() {
    return make_list_directory_generator(_fd);
}

subscription<directory_entry>
posix_file_impl::list_directory(std::function<future<> (directory_entry de)> next) {
    static constexpr size_t buffer_size = 8192;
    struct work {
        stream<directory_entry> s;
        unsigned current = 0;
        unsigned total = 0;
        bool eof = false;
        int error = 0;
        char buffer[buffer_size];
    };

    // While it would be natural to use fdopendir()/readdir(),
    // our syscall thread pool doesn't support malloc(), which is
    // required for this to work.  So resort to using getdents()
    // instead.

    auto w = make_lw_shared<work>();
    auto ret = w->s.listen(std::move(next));
    // List the directory asynchronously in the background.
    // Caller synchronizes using the returned subscription.
    (void)w->s.started().then([w, this] {
        auto eofcond = [w] { return w->eof; };
        return do_until(eofcond, [w, this] {
            if (w->current == w->total) {
                return read_directory(_fd, w->buffer, buffer_size).then([w] (auto size) {
                    if (size == 0) {
                        w->eof = true;
                    } else {
                        w->current = 0;
                        w->total = size;
                    }
                });
            }
            auto start = w->buffer + w->current;
            auto de = reinterpret_cast<linux_dirent64*>(start);
            w->current += de->d_reclen;
            sstring name = de->d_name;
            if (name == "." || name == "..") {
                return make_ready_future<>();
            }
            std::optional<directory_entry_type> type = dirent_type(*de);
            return w->s.produce({std::move(name), type});
        });
    }).then([w] {
        w->s.close();
    }).handle_exception([] (std::exception_ptr ignored) {});
    return ret;
}

future<size_t>
posix_file_impl::do_write_dma(uint64_t pos, const void* buffer, size_t len, io_intent* intent) noexcept {
    auto req = internal::io_request::make_write(_fd, pos, buffer, len, _nowait_works);
    return _io_queue.submit_io_write(len, std::move(req), intent);
}

future<size_t>
posix_file_impl::do_write_dma(uint64_t pos, std::vector<iovec> iov, io_intent* intent) noexcept {
    auto len = internal::sanitize_iovecs(iov, _disk_write_dma_alignment);
    auto req = internal::io_request::make_writev(_fd, pos, iov, _nowait_works);
    return _io_queue.submit_io_write(len, std::move(req), intent, std::move(iov));
}

future<size_t>
posix_file_impl::do_read_dma(uint64_t pos, void* buffer, size_t len, io_intent* intent) noexcept {
    auto req = internal::io_request::make_read(_fd, pos, buffer, len, _nowait_works);
    return _io_queue.submit_io_read(len, std::move(req), intent);
}

future<size_t>
posix_file_impl::do_read_dma(uint64_t pos, std::vector<iovec> iov, io_intent* intent) noexcept {
    auto len = internal::sanitize_iovecs(iov, _disk_read_dma_alignment);
    auto req = internal::io_request::make_readv(_fd, pos, iov, _nowait_works);
    return _io_queue.submit_io_read(len, std::move(req), intent, std::move(iov));
}

future<size_t>
posix_file_real_impl::write_dma(uint64_t pos, const void* buffer, size_t len, io_intent* intent) noexcept {
    return posix_file_impl::do_write_dma(pos, buffer, len, intent);
}

future<size_t>
posix_file_real_impl::write_dma(uint64_t pos, std::vector<iovec> iov, io_intent* intent) noexcept {
    return posix_file_impl::do_write_dma(pos, std::move(iov), intent);
}

future<size_t>
posix_file_real_impl::read_dma(uint64_t pos, void* buffer, size_t len, io_intent* intent) noexcept {
    return posix_file_impl::do_read_dma(pos, buffer, len, intent);
}

future<size_t>
posix_file_real_impl::read_dma(uint64_t pos, std::vector<iovec> iov, io_intent* intent) noexcept {
    return posix_file_impl::do_read_dma(pos, std::move(iov), intent);
}

future<temporary_buffer<uint8_t>>
posix_file_impl::dma_read_bulk(uint64_t offset, size_t range_size, io_intent* intent) noexcept {
    auto front = offset & (_disk_read_dma_alignment - 1);
    offset -= front;
    range_size += front;

    temporary_buffer<uint8_t> buf = temporary_buffer<uint8_t>::aligned(
            _memory_dma_alignment, align_up(range_size, size_t(_disk_read_dma_alignment)));

    //
    // First, try to read directly into the buffer. Most of the reads will
    // end here.
    //
    size_t pos = co_await read_dma_one(offset, buf.get_write(), buf.size(), intent);

    //
    // If we haven't read all required data at once -
    // start read-copy sequence. We can't continue with direct reads
    // into the previously allocated buffer here since we have to ensure
    // the aligned read length and thus the aligned destination buffer
    // size.
    //
    // The copying will actually take place only if there was a HW glitch.
    // In EOF case or in case of a persistent I/O error the only overhead is
    // an extra allocation.
    //
    while (pos < range_size) {
        auto buf1 = co_await read_maybe_eof(offset + pos, range_size - pos, intent);
        if (!buf1.size()) {
            break;
        }

        auto to_copy = std::min(buf.size() - pos, buf1.size());
        std::memcpy(buf.get_write() + pos, buf1.get(), to_copy);
        pos += to_copy;
    }
    //
    // If we are here we are promised to have read some bytes beyond
    // "front" so we may trim straight away.
    //
    if (pos > front) {
        buf.trim(pos);
        buf.trim_front(front);
    } else {
        buf.trim(0);
    }
    co_return std::move(buf);
}

future<temporary_buffer<uint8_t>>
posix_file_impl::read_maybe_eof(uint64_t pos, size_t len, io_intent* intent) {
    //
    // We have to allocate a new aligned buffer to make sure we don't get
    // an EINVAL error due to unaligned destination buffer.
    //
    temporary_buffer<uint8_t> buf = temporary_buffer<uint8_t>::aligned(
               _memory_dma_alignment, align_up(len, size_t(_disk_read_dma_alignment)));

    // try to read a single bulk from the given position
    auto dst = buf.get_write();
    auto buf_size = buf.size();
    try {
        size_t size = co_await read_dma_one(pos, dst, buf_size, intent);
        buf.trim(size);

        co_return std::move(buf);
    } catch (std::system_error& e) {
        //
        // TODO: implement a non-trowing file_impl::dma_read() interface to
        //       avoid the exceptions throwing in a good flow completely.
        //       Otherwise for users that don't want to care about the
        //       underlying file size and preventing the attempts to read
        //       bytes beyond EOF there will always be at least one
        //       exception throwing at the file end for files with unaligned
        //       length.
        //
        if (e.code().value() == EINVAL) {
            buf.trim(0);
            co_return std::move(buf);
        } else {
            throw;
        }
    }
}

static bool blockdev_gen_nowait_works = internal::kernel_uname().whitelisted({"4.13"});
static bool blockdev_md_nowait_works = internal::kernel_uname().whitelisted({"5.17"});

static bool blockdev_nowait_works(dev_t device_id) {
    if (major(device_id) == MD_MAJOR) {
        return blockdev_md_nowait_works;
    }

    return blockdev_gen_nowait_works;
}

future<>
blockdev_file_impl::truncate(uint64_t length) noexcept {
    return make_ready_future<>();
}

future<>
blockdev_file_impl::discard(uint64_t offset, uint64_t length) noexcept {
    auto sr = co_await engine()._thread_pool->submit<syscall_result<int>>(
            internal::thread_pool_submit_reason::file_operation, [this, offset, length] () mutable {
        uint64_t range[2] { offset, length };
        return wrap_syscall<int>(::ioctl(_fd, BLKDISCARD, &range));
    });
    sr.throw_if_error();
}

future<>
blockdev_file_impl::allocate(uint64_t position, uint64_t length) noexcept {
    // nothing to do for block device
    return make_ready_future<>();
}

future<size_t>
blockdev_file_impl::write_dma(uint64_t pos, const void* buffer, size_t len, io_intent* intent) noexcept {
    return posix_file_impl::do_write_dma(pos, buffer, len, intent);
}

future<size_t>
blockdev_file_impl::write_dma(uint64_t pos, std::vector<iovec> iov, io_intent* intent) noexcept {
    return posix_file_impl::do_write_dma(pos, std::move(iov), intent);
}

future<size_t>
blockdev_file_impl::read_dma(uint64_t pos, void* buffer, size_t len, io_intent* intent) noexcept {
    return posix_file_impl::do_read_dma(pos, buffer, len, intent);
}

future<size_t>
blockdev_file_impl::read_dma(uint64_t pos, std::vector<iovec> iov, io_intent* intent) noexcept {
    return posix_file_impl::do_read_dma(pos, std::move(iov), intent);
}

append_challenged_posix_file_impl::append_challenged_posix_file_impl(int fd, open_flags f, file_open_options options, const internal::fs_info& fsi, dev_t device_id)
        : posix_file_impl(fd, f, options, device_id, fsi)
        , _max_size_changing_ops(fsi.append_concurrency)
        , _fsync_is_exclusive(fsi.fsync_is_exclusive)
        , _sloppy_size(options.sloppy_size)
        , _sloppy_size_hint(align_up<uint64_t>(options.sloppy_size_hint, _disk_write_dma_alignment))
{
    auto r = ::lseek(fd, 0, SEEK_END);
    throw_system_error_on(r == -1);
    _committed_size = _logical_size = r;
}

append_challenged_posix_file_impl::~append_challenged_posix_file_impl() {
    // If the file has not been closed we risk having running tasks
    // that will try to access freed memory.
    //
    // It is safe to destory it if nothing is queued.
    // Note that posix_file_impl::~posix_file_impl auto-closes the file descriptor.
    SEASTAR_ASSERT(_q.empty() && (_logical_size == _committed_size || _closing_state == state::closed));
}

bool
append_challenged_posix_file_impl::must_run_alone(const op& candidate) const noexcept {
    // checks if candidate is a non-write, size-changing operation.
    return (candidate.type == opcode::truncate)
            || (candidate.type == opcode::allocate)
            || (candidate.type == opcode::flush && (_fsync_is_exclusive || _sloppy_size));
}

bool
append_challenged_posix_file_impl::appending_write(const op& candidate) const noexcept {
    return (candidate.type == opcode::write) &&
            (candidate.pos + candidate.len > _committed_size);
}

bool
append_challenged_posix_file_impl::size_changing(const op& candidate) const noexcept {
    return appending_write(candidate) || must_run_alone(candidate);
}

bool
append_challenged_posix_file_impl::may_dispatch(const op& candidate) const noexcept {
    if (size_changing(candidate)) {
        return !_current_size_changing_ops && !_current_non_size_changing_ops;
    } else {
        return !_current_size_changing_ops;
    }
}

void
append_challenged_posix_file_impl::dispatch(op& candidate) noexcept {
    unsigned* op_counter = size_changing(candidate)
            ? &_current_size_changing_ops : &_current_non_size_changing_ops;
    ++*op_counter;
    // FIXME: future is discarded
    (void)candidate.run().then([me = shared_from_this(), op_counter] {
        --*op_counter;
        me->process_queue();
    });
}

int append_challenged_posix_file_impl::truncate_sync(uint64_t length) noexcept {
    int r = ::ftruncate(_fd, length);
    if (r != -1) {
        _committed_size = length;
    }
    return r;
}

void append_challenged_posix_file_impl::truncate_to_logical_size() {
    auto r = truncate_sync(_logical_size);
    if (r == -1) {
        throw std::system_error(errno, std::system_category(), "truncate");
    }
}

// If we have a bunch of size-extending writes in the queue,
// issue an ftruncate() extending the file size, so they can
// be issued concurrently.
void
append_challenged_posix_file_impl::optimize_queue() noexcept {
    if (_current_non_size_changing_ops || _current_size_changing_ops) {
        // Can't issue an ftruncate() if something is going on
        return;
    }
    auto speculative_size = _committed_size;
    unsigned n_appending_writes = 0;
    for (const auto& op : _q) {
        // stop calculating speculative size after a non-write, size-changing
        // operation is found to prevent an useless truncate from being issued.
        if (must_run_alone(op)) {
            break;
        }

        if (appending_write(op)) {
            speculative_size = std::max(speculative_size, op.pos + op.len);
            ++n_appending_writes;
        }
    }
    if (n_appending_writes > _max_size_changing_ops
            || (n_appending_writes && _sloppy_size)) {
        if (_sloppy_size) {
          if (!_committed_size) {
            speculative_size = std::max(speculative_size, _sloppy_size_hint);
          } else if (speculative_size < 2 * _committed_size) {
            speculative_size = align_up<uint64_t>(2 * _committed_size, _disk_write_dma_alignment);
          }
        }
        // We're all alone, so issuing the ftruncate() in the reactor
        // thread won't block us.
        //
        // Issuing it in the syscall thread is too slow; this can happen
        // every several ops, and the syscall thread latency can be very
        // high.

        truncate_sync(speculative_size);
        // If we failed, the next write will pick it up.
    }
}

void
append_challenged_posix_file_impl::process_queue() noexcept {
    optimize_queue();
    while (!_q.empty() && may_dispatch(_q.front())) {
        op candidate = std::move(_q.front());
        _q.pop_front();
        dispatch(candidate);
    }
    if (may_quit()) {
        _completed.set_value();
        _closing_state = state::closing; // prevents _completed to be signaled again in case of recursion
    }
}

void
append_challenged_posix_file_impl::enqueue_op(op&& op) {
    _q.push_back(std::move(op));
    process_queue();
}

bool
append_challenged_posix_file_impl::may_quit() const noexcept {
    return _closing_state == state::draining && _q.empty() && !_current_non_size_changing_ops &&
           !_current_size_changing_ops;
}

void
append_challenged_posix_file_impl::commit_size(uint64_t size) noexcept {
    _committed_size = std::max(size, _committed_size);
    _logical_size = std::max(size, _logical_size);
}

future<size_t>
append_challenged_posix_file_impl::read_dma(uint64_t pos, void* buffer, size_t len, io_intent* intent) noexcept {
    if (pos >= _logical_size) {
        // yield() avoids tail recursion
        return yield().then([] {
            return size_t(0);
        });
    }
    len = std::min(pos + len, align_up<uint64_t>(_logical_size, _disk_read_dma_alignment)) - pos;
    internal::intent_reference iref(intent);
    return enqueue<size_t>(
        opcode::read,
        pos,
        len,
        [this, pos, buffer, len, iref = std::move(iref)] () mutable {
            return posix_file_impl::do_read_dma(pos, buffer, len, iref.retrieve());
        }
    );
}

future<size_t>
append_challenged_posix_file_impl::read_dma(uint64_t pos, std::vector<iovec> iov, io_intent* intent) noexcept {
    if (pos >= _logical_size) {
        // yield() avoids tail recursion
        return yield().then([] {
            return size_t(0);
        });
    }
    size_t len = 0;
    auto i = iov.begin();
    auto aligned_logical_size = align_up<uint64_t>(_logical_size, _disk_read_dma_alignment);
    while (i != iov.end() && pos + len + i->iov_len <= aligned_logical_size) {
        len += i++->iov_len;
    }
    if (i != iov.end()) {
        auto last_len = pos + len + i->iov_len - aligned_logical_size;
        if (last_len) {
            i++->iov_len = last_len;
        }
        iov.erase(i, iov.end());
    }
    internal::intent_reference iref(intent);
    return enqueue<size_t>(
        opcode::read,
        pos,
        len,
        [this, pos, iov = std::move(iov), iref = std::move(iref)] () mutable {
            return posix_file_impl::do_read_dma(pos, std::move(iov), iref.retrieve());
        }
    );
}

future<size_t>
append_challenged_posix_file_impl::write_dma(uint64_t pos, const void* buffer, size_t len, io_intent* intent) noexcept {
    internal::intent_reference iref(intent);
    return enqueue<size_t>(
        opcode::write,
        pos,
        len,
        [this, pos, buffer, len, iref = std::move(iref)] {
            return posix_file_impl::do_write_dma(pos, buffer, len, iref.retrieve()).then([this, pos] (size_t ret) {
                commit_size(pos + ret);
                return make_ready_future<size_t>(ret);
            });
        }
    );
}

future<size_t>
append_challenged_posix_file_impl::write_dma(uint64_t pos, std::vector<iovec> iov, io_intent* intent) noexcept {
    auto len = internal::iovec_len(iov);
    internal::intent_reference iref(intent);
    return enqueue<size_t>(
        opcode::write,
        pos,
        len,
        [this, pos, iov = std::move(iov), iref = std::move(iref)] () mutable {
            return posix_file_impl::do_write_dma(pos, std::move(iov), iref.retrieve()).then([this, pos] (size_t ret) {
                commit_size(pos + ret);
                return make_ready_future<size_t>(ret);
            });
        }
    );
}

future<>
append_challenged_posix_file_impl::flush() noexcept {
    if ((!_sloppy_size || _logical_size == _committed_size) && !_fsync_is_exclusive) {
        // FIXME: determine if flush can block concurrent reads or writes
        return posix_file_impl::flush();
    } else {
        return enqueue<>(
            opcode::flush,
            0,
            0,
            [this] () {
                if (_logical_size != _committed_size) {
                    // We're all alone, so can truncate in reactor thread
                    truncate_to_logical_size();
                }
                return posix_file_impl::flush();
            }
        );
    }
}

future<>
append_challenged_posix_file_impl::allocate(uint64_t position, uint64_t length) noexcept {
    return enqueue<>(
            opcode::allocate,
            position,
            length,
            [this, position, length] () {
                return posix_file_impl::allocate(position, length);
            }
        );
}

future<struct stat>
append_challenged_posix_file_impl::stat() noexcept {
    // FIXME: can this conflict with anything?
    auto stat = co_await posix_file_impl::stat();
    stat.st_size = _logical_size;
    co_return stat;
}

future<>
append_challenged_posix_file_impl::truncate(uint64_t length) noexcept {
    return enqueue<>(
        opcode::truncate,
        length,
        0,
        [this, length] () mutable {
            return posix_file_impl::truncate(length).then([this, length] () mutable {
                _committed_size = _logical_size = length;
            });
        }
    );
}

future<uint64_t>
append_challenged_posix_file_impl::size() noexcept {
    return make_ready_future<size_t>(_logical_size);
}

future<>
append_challenged_posix_file_impl::close() noexcept {
    // Caller should have drained all pending I/O
    _closing_state = state::draining;
    process_queue();
    return _completed.get_future().then([this] {
        if (_logical_size != _committed_size) {
            truncate_to_logical_size();
        }
    }).then_wrapped([this] (future<> f) {
        if (f.failed()) {
            report_exception("Closing append_challenged_posix_file_impl failed.", f.get_exception());
        }
        return posix_file_impl::close().finally([this] { _closing_state = state::closed; });
    });
}

posix_file_handle_impl::~posix_file_handle_impl() {
    if (_refcount && _refcount->fetch_add(-1, std::memory_order_relaxed) == 1) {
        ::close(_fd);
        delete _refcount;
    }
}

std::unique_ptr<seastar::file_handle_impl>
posix_file_handle_impl::clone() const {
    auto ret = std::make_unique<posix_file_handle_impl>(_fd, _open_flags, _refcount, _device_id,
            _memory_dma_alignment, _disk_read_dma_alignment, _disk_write_dma_alignment, _disk_overwrite_dma_alignment, _nowait_works, _skip_flush, _aio_fdatasync);
    if (_refcount) {
        _refcount->fetch_add(1, std::memory_order_relaxed);
    }
    return ret;
}

shared_ptr<file_impl>
posix_file_handle_impl::to_file() && {
    auto ret = ::seastar::make_shared<posix_file_real_impl>(_fd, _open_flags, _refcount, _device_id,
            _memory_dma_alignment, _disk_read_dma_alignment, _disk_write_dma_alignment, _disk_overwrite_dma_alignment, _nowait_works, _skip_flush, _aio_fdatasync);
    _fd = -1;
    _refcount = nullptr;
    return ret;
}

// Some kernels can append to xfs filesystems, some cannot; determine
// from kernel version.
static
unsigned
xfs_concurrency_from_kernel_version() {
    // try to see if this is a mainline kernel with xfs append fixed (3.15+)
    // or a RHEL kernel with the backported fix (3.10.0-325.el7+)
    if (internal::kernel_uname().whitelisted({"3.15", "3.10.0-325.el7"})) {
            // Can append, but not concurrently
            return 1;
    }
    // Cannot append at all; need ftrucnate().
    return 0;
}

namespace {

// Query DIO memory alignment using statx (kernel 6.1+)
// Returns the optimal memory buffer alignment for this file descriptor,
// or std::nullopt if statx doesn't support STATX_DIOALIGN
std::optional<size_t>
query_statx_mem_align(int fd) {
#ifndef SEASTAR_STATX_NEEDS_DIO_FIELDS
    // Only call statx if we have the required struct fields
    struct statx stx;
    if (syscall(__NR_statx, fd, "", AT_EMPTY_PATH, STATX_DIOALIGN, &stx) == 0) {
        if (stx.stx_mask & STATX_DIOALIGN) {
            // Use kernel-reported memory alignment (stx_dio_mem_align)
            // Field is available in Linux 6.1+ headers
            return stx.stx_dio_mem_align;
        }
    }
#else
    // Headers don't have the stx_dio_mem_align field, so we can't use it
    // even if the kernel supports it. We'll rely on other fallbacks.
#endif
    return std::nullopt;
}

// Query device-level alignment properties (common to all filesystems)
struct device_alignment_info {
    std::optional<unsigned> memory_alignment;      // from statx
    std::optional<unsigned> physical_block_size;   // from io_queue override
};

device_alignment_info query_device_alignment_info(int fd, dev_t device_id) {
    device_alignment_info info;

    // Query statx for DIO memory alignment (kernel 6.1+)
    info.memory_alignment = query_statx_mem_align(fd);

    // Check for physical_block_size override from io_properties.yaml
    // Note: I/O queues may not be registered for all devices (e.g., tmpfs, test environments)
    if (auto* io_queue = engine().try_get_io_queue(device_id)) {
        info.physical_block_size = io_queue->physical_block_size();
    }

    return info;
}

// Unified function for all filesystem alignment calculations
internal::alignments filesystem_alignments(
    int fd,
    dev_t device_id,
    unsigned block_size,
    unsigned long fs_type  // from statfs.f_type
) {
    auto device_info = query_device_alignment_info(fd, device_id);

    // Initialize with generic defaults for all filesystems
    internal::alignments align = {
        .memory = std::max(device_info.memory_alignment.value_or(4096), internal::min_memory_alignment),
        .disk_read = 512,  // Linux O_DIRECT minimum
        .disk_write = block_size,
        .disk_overwrite = block_size,
    };

    // Override with filesystem-specific alignments if available
    if (fs_type == internal::fs_magic::xfs) {
        dioattr da;
        if (::ioctl(fd, XFS_IOC_DIOINFO, &da) == 0) {
            static bool xfs_with_relaxed_overwrite_alignment = internal::kernel_uname().whitelisted({"5.12"});

            // Override with XFS-specific values
            align.memory = std::max(device_info.memory_alignment.value_or(da.d_mem), internal::min_memory_alignment);
            align.disk_read = da.d_miniosz;
            align.disk_write = std::max<unsigned>(da.d_miniosz, block_size);
            align.disk_overwrite = xfs_with_relaxed_overwrite_alignment ? da.d_miniosz : align.disk_write;
        }
    }

    // Common: apply physical_block_size override for all filesystems
    // This ensures we avoid hardware read-modify-write regardless of filesystem
    if (device_info.physical_block_size) {
        align.disk_write = std::max<unsigned>(align.disk_write, *device_info.physical_block_size);
        align.disk_overwrite = std::max<unsigned>(align.disk_overwrite, *device_info.physical_block_size);
    }

    return align;
}

// Query block device alignment properties using ioctl and statx
internal::alignments
blkdev_alignments(int fd, dev_t device_id) {
    // Query logical block size (smallest addressable unit from hardware)
    // Use BLKSSZGET (not BLKBSZGET) - BLKBSZGET returns the filesystem's
    // buffered I/O block size, while BLKSSZGET returns the actual hardware
    // sector size that the kernel enforces for O_DIRECT alignment
    size_t logical_block_size;
    if (auto ret = ::ioctl(fd, BLKSSZGET, &logical_block_size); ret == -1) {
        throw std::system_error(errno, std::system_category(), "ioctl(BLKSSZGET) failed");
    }

    // Query physical block size (smallest atomic write unit)
    size_t physical_block_size;
    if (auto ret = ::ioctl(fd, BLKPBSZGET, &physical_block_size); ret == -1) {
        throw std::system_error(errno, std::system_category(), "ioctl(BLKPBSZGET) failed");
    }

    // Query device alignment info (statx memory alignment and physical_block_size override)
    auto device_info = query_device_alignment_info(fd, device_id);

    // Apply physical_block_size override from io_properties.yaml if present
    // This is needed because some devices lie about their physical block size
    if (device_info.physical_block_size) {
        physical_block_size = *device_info.physical_block_size;
    }

    // For writes: use physical_block_size to avoid hardware-level read-modify-write
    // - physical_block_size: smallest unit a physical storage device can write atomically (e.g., 4096 bytes for Advanced Format disks)
    // - logical_block_size: smallest unit the storage device can address (typically 512 bytes)
    //
    // The Linux kernel only enforces logical_block_size alignment for O_DIRECT (see block/fops.c:blkdev_dio_invalid).
    // Using physical_block_size avoids RMW at the hardware level.
    return {
        .memory = std::max(static_cast<unsigned>(device_info.memory_alignment.value_or(physical_block_size)), internal::min_memory_alignment),
        .disk_read = static_cast<unsigned>(logical_block_size),  // For reads: use logical_block_size (no performance penalty for reading 512-byte blocks from 4K sector disks)
        .disk_write = static_cast<unsigned>(physical_block_size),
        .disk_overwrite = static_cast<unsigned>(physical_block_size),
    };
}

} // anonymous namespace

future<shared_ptr<file_impl>>
make_file_impl(int fd, file_open_options options, int flags, struct stat st) noexcept {
    if (S_ISBLK(st.st_mode)) {
        try {
            auto align = blkdev_alignments(fd, st.st_rdev);
            internal::fs_info fsi;
            fsi.block_size = align.disk_read; // use logical_block_size for block_size
            fsi.nowait_works = blockdev_nowait_works(st.st_rdev);
            fsi.align = align;
            return make_ready_future<shared_ptr<file_impl>>(make_shared<blockdev_file_impl>(fd, open_flags(flags), options, fsi, st.st_rdev));
        } catch (...) {
            return make_exception_future<shared_ptr<file_impl>>(std::current_exception());
        }
    }

    if (S_ISDIR(st.st_mode)) {
        // Directories don't care about block size, so we need not
        // query it here. Just provide something reasonable.
        internal::fs_info fsi;
        fsi.block_size = 4096;
        fsi.nowait_works = false;
        return make_ready_future<shared_ptr<file_impl>>(make_shared<posix_file_real_impl>(fd, open_flags(flags), options, fsi, st.st_dev));
    }

    auto st_dev = st.st_dev;
    static thread_local std::unordered_map<decltype(st_dev), internal::fs_info> s_fstype;

    auto i = s_fstype.find(st_dev);
    if (i == s_fstype.end()) [[unlikely]] {
        return engine().fstatfs(fd).then([fd, options = std::move(options), flags, st = std::move(st)] (struct statfs sfs) {
            internal::fs_info fsi;
            fsi.block_size = sfs.f_bsize;
            switch (sfs.f_type) {
            case internal::fs_magic::xfs:
                fsi.append_challenged = true;
                static auto xc = xfs_concurrency_from_kernel_version();
                fsi.append_concurrency = xc;
                fsi.fsync_is_exclusive = true;
                fsi.nowait_works = internal::kernel_uname().whitelisted({"4.13"});
                break;
            case internal::fs_magic::nfs:
                fsi.append_challenged = false;
                fsi.append_concurrency = 0;
                fsi.fsync_is_exclusive = false;
                fsi.nowait_works = internal::kernel_uname().whitelisted({"4.13"});
                break;
            case internal::fs_magic::ext4:
                fsi.append_challenged = true;
                fsi.append_concurrency = 0;
                fsi.fsync_is_exclusive = false;
                fsi.nowait_works = internal::kernel_uname().whitelisted({"5.5"});
                break;
            case internal::fs_magic::btrfs:
                fsi.append_challenged = true;
                fsi.append_concurrency = 0;
                fsi.fsync_is_exclusive = true;
                fsi.nowait_works = internal::kernel_uname().whitelisted({"5.9"});
                break;
            case internal::fs_magic::tmpfs:
            case internal::fs_magic::fuse:
                fsi.append_challenged = false;
                fsi.append_concurrency = 999;
                fsi.fsync_is_exclusive = false;
                fsi.nowait_works = false;
                break;
            default:
                fsi.append_challenged = true;
                fsi.append_concurrency = 0;
                fsi.fsync_is_exclusive = true;
                fsi.nowait_works = false;
            }
            fsi.nowait_works &= engine()._cfg.aio_nowait_works;
            fsi.align = filesystem_alignments(fd, st.st_dev, fsi.block_size, sfs.f_type);
            s_fstype.insert(std::make_pair(st.st_dev, std::move(fsi)));
            return make_file_impl(fd, std::move(options), flags, std::move(st));
        });
    }

    try {
        const internal::fs_info& fsi = i->second;
        if (!fsi.append_challenged || options.append_is_unlikely || ((flags & O_ACCMODE) == O_RDONLY)) {
            return make_ready_future<shared_ptr<file_impl>>(make_shared<posix_file_real_impl>(fd, open_flags(flags), std::move(options), fsi, st_dev));
        }
        return make_ready_future<shared_ptr<file_impl>>(make_shared<append_challenged_posix_file_impl>(fd, open_flags(flags), std::move(options), fsi, st_dev));
    } catch(...) {
        return current_exception_as_future<shared_ptr<file_impl>>();
    }
}

file::file(seastar::file_handle&& handle) noexcept
        : _file_impl(std::move(std::move(handle).to_file()._file_impl)) {
}

future<uint64_t> file::size() const noexcept {
  try {
    return _file_impl->size();
  } catch (...) {
    return current_exception_as_future<uint64_t>();
  }
}

future<> file::close() noexcept {
    auto f = std::move(_file_impl);
    return f->close().handle_exception([f = std::move(f)] (std::exception_ptr ex) {
        report_exception("Closing the file failed unexpectedly", std::move(ex));
    });
}

subscription<directory_entry>
file::list_directory(std::function<future<>(directory_entry de)> next) {
    return _file_impl->list_directory(std::move(next));
}

list_directory_generator_type file::experimental_list_directory() {
    return _file_impl->experimental_list_directory();
}

future<int> file::ioctl(uint64_t cmd, void* argp) noexcept {
    return _file_impl->ioctl(cmd, argp);
}

future<int> file::ioctl_short(uint64_t cmd, void* argp) noexcept {
    return _file_impl->ioctl_short(cmd, argp);
}

future<int> file::fcntl(int op, uintptr_t arg) noexcept {
    return _file_impl->fcntl(op, arg);
}

future<int> file::fcntl_short(int op, uintptr_t arg) noexcept {
    return _file_impl->fcntl_short(op, arg);
}

future<> file::set_lifetime_hint_impl(int op, uint64_t hint) noexcept {
    uint64_t arg = hint;
    future<int> f = co_await coroutine::as_future(_file_impl->fcntl(op, (uintptr_t)&arg));
    // Need to handle return value differently from that of fcntl
    if (f.failed()) {
        co_await coroutine::exception(f.get_exception());
    }
}

future<> file::set_inode_lifetime_hint(uint64_t hint) noexcept {
    return set_lifetime_hint_impl(F_SET_RW_HINT, hint);
}

future<uint64_t> file::get_lifetime_hint_impl(int op) noexcept {
    uint64_t arg;
    future<int> f = co_await coroutine::as_future(_file_impl->fcntl(op, (uintptr_t)&arg));
    if (f.failed()) {
        co_return coroutine::exception(f.get_exception());
    }
    co_return arg;
}

future<uint64_t> file::get_inode_lifetime_hint() noexcept {
    return get_lifetime_hint_impl(F_GET_RW_HINT);
}

future<temporary_buffer<uint8_t>>
file::dma_read_bulk_impl(uint64_t offset, size_t range_size, io_intent* intent) noexcept {
  try {
    return _file_impl->dma_read_bulk(offset, range_size, intent);
  } catch (...) {
    return current_exception_as_future<temporary_buffer<uint8_t>>();
  }
}

future<> file::discard(uint64_t offset, uint64_t length) noexcept {
  try {
    return _file_impl->discard(offset, length);
  } catch (...) {
    return current_exception_as_future();
  }
}

future<> file::allocate(uint64_t position, uint64_t length) noexcept {
  try {
    return _file_impl->allocate(position, length);
  } catch (...) {
    return current_exception_as_future();
  }
}

future<> file::truncate(uint64_t length) noexcept {
  try {
    return _file_impl->truncate(length);
  } catch (...) {
    return current_exception_as_future();
  }
}

future<struct stat> file::stat() noexcept {
  try {
    return _file_impl->stat();
  } catch (...) {
    return current_exception_as_future<struct stat>();
  }
}

future<struct stat> file::statat(std::string_view name, int flags) noexcept {
    try {
        return _file_impl->statat(name, flags);
    } catch (...) {
        return current_exception_as_future<struct stat>();
    }
}

future<> file::flush() noexcept {
  try {
    return _file_impl->flush();
  } catch (...) {
    return current_exception_as_future();
  }
}

future<size_t> file::dma_write_impl(uint64_t pos, std::vector<iovec> iov, io_intent* intent) noexcept {
  try {
    return _file_impl->write_dma(pos, std::move(iov), intent);
  } catch (...) {
    return current_exception_as_future<size_t>();
  }
}

future<size_t>
file::dma_write_impl(uint64_t pos, const uint8_t* buffer, size_t len, io_intent* intent) noexcept {
  try {
    return _file_impl->write_dma(pos, buffer, len, intent);
  } catch (...) {
    return current_exception_as_future<size_t>();
  }
}

future<size_t> file::dma_read_impl(uint64_t pos, std::vector<iovec> iov, io_intent* intent) noexcept {
  try {
    return _file_impl->read_dma(pos, std::move(iov), intent);
  } catch (...) {
    return current_exception_as_future<size_t>();
  }
}

future<temporary_buffer<uint8_t>>
file::dma_read_exactly_impl(uint64_t pos, size_t len, io_intent* intent) noexcept {
    auto buf = co_await dma_read_impl(pos, len, intent);
    if (buf.size() < len) {
        throw eof_error();
    }

    co_return std::move(buf);
}

future<temporary_buffer<uint8_t>>
file::dma_read_impl(uint64_t pos, size_t len, io_intent* intent) noexcept {
    temporary_buffer<uint8_t> buf = co_await dma_read_bulk_impl(pos, len, intent);
    if (len < buf.size()) {
        buf.trim(len);
    }

    co_return std::move(buf);
}

future<size_t>
file::dma_read_impl(uint64_t aligned_pos, uint8_t* aligned_buffer, size_t aligned_len, io_intent* intent) noexcept {
  try {
    return _file_impl->read_dma(aligned_pos, aligned_buffer, aligned_len, intent);
  } catch (...) {
    return current_exception_as_future<size_t>();
  }
}

seastar::file_handle
file::dup() {
    return seastar::file_handle(_file_impl->dup());
}

file_impl* file_impl::get_file_impl(file& f) {
    return f._file_impl.get();
}

std::unique_ptr<seastar::file_handle_impl>
file_impl::dup() {
    throw std::runtime_error("this file type cannot be duplicated");
}

static list_directory_generator_type make_list_directory_fallback_generator(file_impl& me) {
    auto ents = make_lw_shared<queue<std::optional<directory_entry>>>(list_directory_generator_buffer_size);
    auto lister = me.list_directory([ents] (directory_entry de) {
        return ents->push_eventually(std::move(de));
    });
    auto done = lister.done().finally([ents] {
        return ents->push_eventually(std::nullopt);
    });
    auto abort = defer([ents, &done] () mutable noexcept {
        ents->abort(std::make_exception_ptr(std::runtime_error("generator abandoned")));
        engine().run_in_background(std::move(done).handle_exception([] (std::exception_ptr ignored) {}));
    });

    while (true) {
        auto de = co_await ents->pop_eventually();
        if (!de) {
            break;
        }
        co_yield *de;
    }

    abort.cancel();
    co_await std::move(done);
}

list_directory_generator_type file_impl::experimental_list_directory() {
    return make_list_directory_fallback_generator(*this);
}

future<int> file_impl::ioctl(uint64_t cmd, void* argp) noexcept {
    return make_exception_future<int>(std::runtime_error("this file type does not support ioctl"));
}

future<int> file_impl::ioctl_short(uint64_t cmd, void* argp) noexcept {
    return make_exception_future<int>(std::runtime_error("this file type does not support ioctl_short"));
}

future<int> file_impl::fcntl(int op, uintptr_t arg) noexcept {
    return make_exception_future<int>(std::runtime_error("this file type does not support fcntl"));
}

future<int> file_impl::fcntl_short(int op, uintptr_t arg) noexcept {
    return make_exception_future<int>(std::runtime_error("this file type does not support fcntl_short"));
}

future<struct stat> file_impl::statat(std::string_view name, int flags) {
    return make_exception_future<struct stat>(std::runtime_error("this file type does not support statat"));
}

future<file> open_file_dma(std::string_view name, open_flags flags) noexcept {
    return engine().open_file_dma(name, flags, file_open_options());
}

future<file> open_file_dma(std::string_view name, open_flags flags, file_open_options options) noexcept {
    return engine().open_file_dma(name, flags, std::move(options));
}

future<file> open_directory(std::string_view name) noexcept {
    return engine().open_directory(name);
}

namespace testing {

shared_ptr<append_challenged_posix_file_impl>
make_append_challenged_posix_file(file_desc& fd, unsigned concurrency, bool fsync_is_exclusive, std::optional<size_t> sloppy_size) {
    internal::fs_info fsi {
        .block_size = 4096,
        .append_challenged = true,
        .append_concurrency = concurrency,
        .fsync_is_exclusive = fsync_is_exclusive,
        .nowait_works = true,
        .align = std::nullopt,
    };
    // device number can be any value, reactor would just pick "fallback" queue
    // that will be unused anyway, because no real IO is supposed to happen
    constexpr dev_t dev = 0;
    return make_shared<append_challenged_posix_file_impl>(fd.get(), open_flags::rw, file_open_options{}, fsi, dev);
}

}

}
