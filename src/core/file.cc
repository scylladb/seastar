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

#define __user /* empty */  // for xfs includes, below

#include <sys/syscall.h>
#include <dirent.h>
#include <linux/types.h> // for xfs, below
#include <sys/ioctl.h>
#include <xfs/linux.h>
#define min min    /* prevent xfs.h from defining min() as a macro */
#include <xfs/xfs.h>
#undef min
#include <boost/range/numeric.hpp>
#include <boost/range/adaptor/transformed.hpp>
#include <seastar/core/reactor.hh>
#include <seastar/core/file.hh>
#include <seastar/core/report_exception.hh>
#include <seastar/core/linux-aio.hh>
#include "core/file-impl.hh"
#include "core/syscall_result.hh"
#include "core/thread_pool.hh"
#include "core/uname.hh"

namespace seastar {

using namespace internal;
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

posix_file_impl::posix_file_impl(int fd, open_flags f, file_open_options options, io_queue* ioq)
        : _io_queue(ioq)
        , _open_flags(f)
        , _fd(fd)
{
    query_dma_alignment();
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

void
posix_file_impl::query_dma_alignment() {
    dioattr da;
    auto r = ioctl(_fd, XFS_IOC_DIOINFO, &da);
    if (r == 0) {
        _memory_dma_alignment = da.d_mem;
        _disk_read_dma_alignment = da.d_miniosz;
        // xfs wants at least the block size for writes
        // FIXME: really read the block size
        _disk_write_dma_alignment = std::max<unsigned>(da.d_miniosz, 4096);
    }
}

std::unique_ptr<seastar::file_handle_impl>
posix_file_impl::dup() {
    if (!_refcount) {
        _refcount = new std::atomic<unsigned>(1u);
    }
    auto ret = std::make_unique<posix_file_handle_impl>(_fd, _open_flags, _refcount, _io_queue);
    _refcount->fetch_add(1, std::memory_order_relaxed);
    return ret;
}

posix_file_impl::posix_file_impl(int fd, open_flags f, std::atomic<unsigned>* refcount, io_queue *ioq)
        : _refcount(refcount), _io_queue(ioq), _open_flags(f), _fd(fd) {
}

future<>
posix_file_impl::flush(void) {
    if ((_open_flags & open_flags::dsync) != open_flags{}) {
        return make_ready_future<>();
    }
    return engine().fdatasync(_fd);
}

future<struct stat>
posix_file_impl::stat(void) {
    return engine()._thread_pool->submit<syscall_result_extra<struct stat>>([this] {
        struct stat st;
        auto ret = ::fstat(_fd, &st);
        return wrap_syscall(ret, st);
    }).then([] (syscall_result_extra<struct stat> ret) {
        ret.throw_if_error();
        return make_ready_future<struct stat>(ret.extra);
    });
}

future<>
posix_file_impl::truncate(uint64_t length) {
    return engine()._thread_pool->submit<syscall_result<int>>([this, length] {
        return wrap_syscall<int>(::ftruncate(_fd, length));
    }).then([] (syscall_result<int> sr) {
        sr.throw_if_error();
        return make_ready_future<>();
    });
}

future<>
posix_file_impl::discard(uint64_t offset, uint64_t length) {
    return engine()._thread_pool->submit<syscall_result<int>>([this, offset, length] () mutable {
        return wrap_syscall<int>(::fallocate(_fd, FALLOC_FL_PUNCH_HOLE|FALLOC_FL_KEEP_SIZE,
            offset, length));
    }).then([] (syscall_result<int> sr) {
        sr.throw_if_error();
        return make_ready_future<>();
    });
}

future<>
posix_file_impl::allocate(uint64_t position, uint64_t length) {
#ifdef FALLOC_FL_ZERO_RANGE
    // FALLOC_FL_ZERO_RANGE is fairly new, so don't fail if it's not supported.
    static bool supported = true;
    if (!supported) {
        return make_ready_future<>();
    }
    return engine()._thread_pool->submit<syscall_result<int>>([this, position, length] () mutable {
        auto ret = ::fallocate(_fd, FALLOC_FL_ZERO_RANGE|FALLOC_FL_KEEP_SIZE, position, length);
        if (ret == -1 && errno == EOPNOTSUPP) {
            ret = 0;
            supported = false; // Racy, but harmless.  At most we issue an extra call or two.
        }
        return wrap_syscall<int>(ret);
    }).then([] (syscall_result<int> sr) {
        sr.throw_if_error();
        return make_ready_future<>();
    });
#else
    return make_ready_future<>();
#endif
}

future<uint64_t>
posix_file_impl::size() {
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
    auto closed = [fd] () noexcept {
        try {
            return engine()._thread_pool->submit<syscall_result<int>>([fd] {
                return wrap_syscall<int>(::close(fd));
            });
        } catch (...) {
            report_exception("Running ::close() in reactor thread, submission failed with exception", std::current_exception());
            return make_ready_future<syscall_result<int>>(wrap_syscall<int>(::close(fd)));
        }
    }();
    return closed.then([] (syscall_result<int> sr) {
        sr.throw_if_error();
    });
}

future<uint64_t>
blockdev_file_impl::size(void) {
    return engine()._thread_pool->submit<syscall_result_extra<size_t>>([this] {
        uint64_t size;
        int ret = ::ioctl(_fd, BLKGETSIZE64, &size);
        return wrap_syscall(ret, size);
    }).then([] (syscall_result_extra<uint64_t> ret) {
        ret.throw_if_error();
        return make_ready_future<uint64_t>(ret.extra);
    });
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

    // From getdents(2):
    struct linux_dirent64 {
        ino64_t        d_ino;    /* 64-bit inode number */
        off64_t        d_off;    /* 64-bit offset to next structure */
        unsigned short d_reclen; /* Size of this dirent */
        unsigned char  d_type;   /* File type */
        char           d_name[]; /* Filename (null-terminated) */
    };

    auto w = make_lw_shared<work>();
    auto ret = w->s.listen(std::move(next));
    // List the directory asynchronously in the background.
    // Caller synchronizes using the returned subscription.
    (void)w->s.started().then([w, this] {
        auto eofcond = [w] { return w->eof; };
        return do_until(eofcond, [w, this] {
            if (w->current == w->total) {
                return engine()._thread_pool->submit<syscall_result<long>>([w , this] () {
                    auto ret = ::syscall(__NR_getdents64, _fd, reinterpret_cast<linux_dirent64*>(w->buffer), buffer_size);
                    return wrap_syscall(ret);
                }).then([w] (syscall_result<long> ret) {
                    ret.throw_if_error();
                    if (ret.result == 0) {
                        w->eof = true;
                    } else {
                        w->current = 0;
                        w->total = ret.result;
                    }
                });
            }
            auto start = w->buffer + w->current;
            auto de = reinterpret_cast<linux_dirent64*>(start);
            compat::optional<directory_entry_type> type;
            switch (de->d_type) {
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
            w->current += de->d_reclen;
            sstring name = de->d_name;
            if (name == "." || name == "..") {
                return make_ready_future<>();
            }
            return w->s.produce({std::move(name), type});
        });
    }).then([w] {
        w->s.close();
    }).handle_exception([] (std::exception_ptr ignored) {});
    return ret;
}

future<size_t>
posix_file_impl::write_dma(uint64_t pos, const void* buffer, size_t len, const io_priority_class& io_priority_class) {
    return engine().submit_io_write(_io_queue, io_priority_class, len, [fd = _fd, pos, buffer, len] (iocb& io) {
        io = make_write_iocb(fd, pos, const_cast<void*>(buffer), len);
    }).then([] (io_event ev) {
        engine().handle_io_result(ev);
        return make_ready_future<size_t>(size_t(ev.res));
    });
}

future<size_t>
posix_file_impl::write_dma(uint64_t pos, std::vector<iovec> iov, const io_priority_class& io_priority_class) {
    auto len = internal::sanitize_iovecs(iov, _disk_write_dma_alignment);
    auto size = iov.size();
    auto data = iov.data();
    return engine().submit_io_write(_io_queue, io_priority_class, len, [fd = _fd, pos, data, size] (iocb& io) {
        io = make_writev_iocb(fd, pos, data, size);
    }).then([iov = std::move(iov)] (io_event ev) {
        engine().handle_io_result(ev);
        return make_ready_future<size_t>(size_t(ev.res));
    });
}

future<size_t>
posix_file_impl::read_dma(uint64_t pos, void* buffer, size_t len, const io_priority_class& io_priority_class) {
    return engine().submit_io_read(_io_queue, io_priority_class, len, [fd = _fd, pos, buffer, len] (iocb& io) {
        io = make_read_iocb(fd, pos, buffer, len);
    }).then([] (io_event ev) {
        engine().handle_io_result(ev);
        return make_ready_future<size_t>(size_t(ev.res));
    });
}

future<size_t>
posix_file_impl::read_dma(uint64_t pos, std::vector<iovec> iov, const io_priority_class& io_priority_class) {
    auto len = internal::sanitize_iovecs(iov, _disk_read_dma_alignment);
    auto size = iov.size();
    auto data = iov.data();
    return engine().submit_io_read(_io_queue, io_priority_class, len, [fd = _fd, pos, data, size] (iocb& io) {
        io = make_readv_iocb(fd, pos, data, size);
    }).then([iov = std::move(iov)] (io_event ev) {
        engine().handle_io_result(ev);
        return make_ready_future<size_t>(size_t(ev.res));
    });
}

future<temporary_buffer<uint8_t>>
posix_file_impl::dma_read_bulk(uint64_t offset, size_t range_size, const io_priority_class& pc) {
    using tmp_buf_type = typename file::read_state<uint8_t>::tmp_buf_type;

    auto front = offset & (_disk_read_dma_alignment - 1);
    offset -= front;
    range_size += front;

    auto rstate = make_lw_shared<file::read_state<uint8_t>>(offset, front,
                                                       range_size,
                                                       _memory_dma_alignment,
                                                       _disk_read_dma_alignment);

    //
    // First, try to read directly into the buffer. Most of the reads will
    // end here.
    //
    auto read = read_dma(offset, rstate->buf.get_write(),
                         rstate->buf.size(), pc);

    return read.then([rstate, this, &pc] (size_t size) mutable {
        rstate->pos = size;

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
        return do_until(
            [rstate] { return rstate->done(); },
            [rstate, this, &pc] () mutable {
            return read_maybe_eof(
                rstate->cur_offset(), rstate->left_to_read(), pc).then(
                    [rstate] (auto buf1) mutable {
                if (buf1.size()) {
                    rstate->append_new_data(buf1);
                } else {
                    rstate->eof = true;
                }

                return make_ready_future<>();
            });
        }).then([rstate] () mutable {
            //
            // If we are here we are promised to have read some bytes beyond
            // "front" so we may trim straight away.
            //
            rstate->trim_buf_before_ret();
            return make_ready_future<tmp_buf_type>(std::move(rstate->buf));
        });
    });
}

future<temporary_buffer<uint8_t>>
posix_file_impl::read_maybe_eof(uint64_t pos, size_t len, const io_priority_class& pc) {
    //
    // We have to allocate a new aligned buffer to make sure we don't get
    // an EINVAL error due to unaligned destination buffer.
    //
    temporary_buffer<uint8_t> buf = temporary_buffer<uint8_t>::aligned(
               _memory_dma_alignment, align_up(len, size_t(_disk_read_dma_alignment)));

    // try to read a single bulk from the given position
    auto dst = buf.get_write();
    auto buf_size = buf.size();
    return read_dma(pos, dst, buf_size, pc).then_wrapped(
            [buf = std::move(buf)](future<size_t> f) mutable {
        try {
            size_t size = std::get<0>(f.get());

            buf.trim(size);

            return std::move(buf);
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
                return std::move(buf);
            } else {
                throw;
            }
        }
    });
}

blockdev_file_impl::blockdev_file_impl(int fd, open_flags f, file_open_options options, io_queue *ioq)
        : posix_file_impl(fd, f, options, ioq) {
}

future<>
blockdev_file_impl::truncate(uint64_t length) {
    return make_ready_future<>();
}

future<>
blockdev_file_impl::discard(uint64_t offset, uint64_t length) {
    return engine()._thread_pool->submit<syscall_result<int>>([this, offset, length] () mutable {
        uint64_t range[2] { offset, length };
        return wrap_syscall<int>(::ioctl(_fd, BLKDISCARD, &range));
    }).then([] (syscall_result<int> sr) {
        sr.throw_if_error();
        return make_ready_future<>();
    });
}

future<>
blockdev_file_impl::allocate(uint64_t position, uint64_t length) {
    // nothing to do for block device
    return make_ready_future<>();
}

append_challenged_posix_file_impl::append_challenged_posix_file_impl(int fd, open_flags f, file_open_options options,
        unsigned max_size_changing_ops, bool fsync_is_exclusive, io_queue* ioq)
        : posix_file_impl(fd, f, options, ioq)
        , _max_size_changing_ops(max_size_changing_ops)
        , _fsync_is_exclusive(fsync_is_exclusive) {
    auto r = ::lseek(fd, 0, SEEK_END);
    throw_system_error_on(r == -1);
    _committed_size = _logical_size = r;
    _sloppy_size = options.sloppy_size;
    auto hint = align_up<uint64_t>(options.sloppy_size_hint, _disk_write_dma_alignment);
    if (_sloppy_size && _committed_size < hint) {
        auto r = ::ftruncate(_fd, hint);
        // We can ignore errors, since it's just a hint.
        if (r != -1) {
            _committed_size = hint;
        }
    }
}

append_challenged_posix_file_impl::~append_challenged_posix_file_impl() {
    // If the file has not been closed we risk having running tasks
    // that will try to access freed memory.
    assert(_closing_state == state::closed);
}

bool
append_challenged_posix_file_impl::must_run_alone(const op& candidate) const noexcept {
    // checks if candidate is a non-write, size-changing operation.
    return (candidate.type == opcode::truncate)
            || (candidate.type == opcode::flush && (_fsync_is_exclusive || _sloppy_size));
}

bool
append_challenged_posix_file_impl::size_changing(const op& candidate) const noexcept {
    return (candidate.type == opcode::write && candidate.pos + candidate.len > _committed_size)
            || must_run_alone(candidate);
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
        if (op.type == opcode::write && op.pos + op.len > _committed_size) {
            speculative_size = std::max(speculative_size, op.pos + op.len);
            ++n_appending_writes;
        }
    }
    if (n_appending_writes > _max_size_changing_ops
            || (n_appending_writes && _sloppy_size)) {
        if (_sloppy_size && speculative_size < 2 * _committed_size) {
            speculative_size = align_up<uint64_t>(2 * _committed_size, _disk_write_dma_alignment);
        }
        // We're all alone, so issuing the ftruncate() in the reactor
        // thread won't block us.
        //
        // Issuing it in the syscall thread is too slow; this can happen
        // every several ops, and the syscall thread latency can be very
        // high.
        auto r = ::ftruncate(_fd, speculative_size);
        if (r != -1) {
            _committed_size = speculative_size;
            // If we failed, the next write will pick it up.
        }
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
append_challenged_posix_file_impl::enqueue(op&& op) {
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
append_challenged_posix_file_impl::read_dma(uint64_t pos, void* buffer, size_t len, const io_priority_class& pc) {
    if (pos >= _logical_size) {
        // later() avoids tail recursion
        return later().then([] {
            return size_t(0);
        });
    }
    len = std::min(pos + len, align_up<uint64_t>(_logical_size, _disk_read_dma_alignment)) - pos;
    auto pr = make_lw_shared(promise<size_t>());
    enqueue({
        opcode::read,
        pos,
        len,
        [this, pr, pos, buffer, len, &pc] {
            return futurize_apply([this, pos, buffer, len, &pc] () mutable {
                return posix_file_impl::read_dma(pos, buffer, len, pc);
            }).then_wrapped([pr] (future<size_t> f) {
                f.forward_to(std::move(*pr));
            });
        }
    });
    return pr->get_future();
}

future<size_t>
append_challenged_posix_file_impl::read_dma(uint64_t pos, std::vector<iovec> iov, const io_priority_class& pc) {
    if (pos >= _logical_size) {
        // later() avoids tail recursion
        return later().then([] {
            return size_t(0);
        });
    }
    size_t len = 0;
    auto i = iov.begin();
    while (i != iov.end() && pos + len + i->iov_len <= _logical_size) {
        len += i++->iov_len;
    }
    auto aligned_logical_size = align_up<uint64_t>(_logical_size, _disk_read_dma_alignment);
    if (i != iov.end()) {
        auto last_len = pos + len + i->iov_len - aligned_logical_size;
        if (last_len) {
            i++->iov_len = last_len;
        }
        iov.erase(i, iov.end());
    }
    auto pr = make_lw_shared(promise<size_t>());
    enqueue({
        opcode::read,
        pos,
        len,
        [this, pr, pos, iov = std::move(iov), &pc] () mutable {
            return futurize_apply([this, pos, iov = std::move(iov), &pc] () mutable {
                return posix_file_impl::read_dma(pos, std::move(iov), pc);
            }).then_wrapped([pr] (future<size_t> f) {
                f.forward_to(std::move(*pr));
            });
        }
    });
    return pr->get_future();
}

future<size_t>
append_challenged_posix_file_impl::write_dma(uint64_t pos, const void* buffer, size_t len, const io_priority_class& pc) {
    auto pr = make_lw_shared(promise<size_t>());
    enqueue({
        opcode::write,
        pos,
        len,
        [this, pr, pos, buffer, len, &pc] {
            return futurize_apply([this, pos, buffer, len, &pc] () mutable {
                return posix_file_impl::write_dma(pos, buffer, len, pc);
            }).then_wrapped([this, pos, pr] (future<size_t> f) {
                if (!f.failed()) {
                    auto ret = f.get0();
                    commit_size(pos + ret);
                    // Can't use forward_to(), because future::get0() invalidates the future.
                    pr->set_value(ret);
                } else {
                    f.forward_to(std::move(*pr));
                }
            });
        }
    });
    return pr->get_future();
}

future<size_t>
append_challenged_posix_file_impl::write_dma(uint64_t pos, std::vector<iovec> iov, const io_priority_class& pc) {
    auto pr = make_lw_shared(promise<size_t>());
    auto len = boost::accumulate(iov | boost::adaptors::transformed(std::mem_fn(&iovec::iov_len)), size_t(0));
    enqueue({
        opcode::write,
        pos,
        len,
        [this, pr, pos, iov = std::move(iov), &pc] () mutable {
            return futurize_apply([this, pos, iov = std::move(iov), &pc] () mutable {
                return posix_file_impl::write_dma(pos, std::move(iov), pc);
            }).then_wrapped([this, pos, pr] (future<size_t> f) {
                if (!f.failed()) {
                    auto ret = f.get0();
                    commit_size(pos + ret);
                    // Can't use forward_to(), because future::get0() invalidates the future.
                    pr->set_value(ret);
                } else {
                    f.forward_to(std::move(*pr));
                }
            });
        }
    });
    return pr->get_future();
}

future<>
append_challenged_posix_file_impl::flush() {
    if ((!_sloppy_size || _logical_size == _committed_size) && !_fsync_is_exclusive) {
        // FIXME: determine if flush can block concurrent reads or writes
        return posix_file_impl::flush();
    } else {
        auto pr = make_lw_shared(promise<>());
        enqueue({
            opcode::flush,
            0,
            0,
            [this, pr] () {
                return futurize_apply([this] {
                    if (_logical_size != _committed_size) {
                        // We're all alone, so can truncate in reactor thread
                        auto r = ::ftruncate(_fd, _logical_size);
                        throw_system_error_on(r == -1);
                        _committed_size = _logical_size;
                    }
                    return posix_file_impl::flush();
                }).then_wrapped([pr] (future<> f) {
                    f.forward_to(std::move(*pr));
                });
            }
        });
        return pr->get_future();
    }
}

future<struct stat>
append_challenged_posix_file_impl::stat() {
    // FIXME: can this conflict with anything?
    return posix_file_impl::stat().then([this] (struct stat stat) {
        stat.st_size = _logical_size;
        return stat;
    });
}

future<>
append_challenged_posix_file_impl::truncate(uint64_t length) {
    auto pr = make_lw_shared(promise<>());
    enqueue({
        opcode::truncate,
        length,
        0,
        [this, pr, length] () mutable {
            return futurize_apply([this, length] {
                return posix_file_impl::truncate(length);
            }).then_wrapped([this, pr, length] (future<> f) {
                if (!f.failed()) {
                    _committed_size = _logical_size = length;
                }
                f.forward_to(std::move(*pr));
            });
        }
    });
    return pr->get_future();
}

future<uint64_t>
append_challenged_posix_file_impl::size() {
    return make_ready_future<size_t>(_logical_size);
}

future<>
append_challenged_posix_file_impl::close() noexcept {
    // Caller should have drained all pending I/O
    _closing_state = state::draining;
    process_queue();
    return _completed.get_future().then([this] {
        if (_logical_size != _committed_size) {
            auto r = ::ftruncate(_fd, _logical_size);
            if (r != -1) {
                _committed_size = _logical_size;
            }
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
    auto ret = std::make_unique<posix_file_handle_impl>(_fd, _open_flags, _refcount, _io_queue);
    if (_refcount) {
        _refcount->fetch_add(1, std::memory_order_relaxed);
    }
    return ret;
}

shared_ptr<file_impl>
posix_file_handle_impl::to_file() && {
    auto ret = ::seastar::make_shared<posix_file_impl>(_fd, _open_flags, _refcount, _io_queue);
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
    if (kernel_uname().whitelisted({"3.15", "3.10.0-325.el7"})) {
            // Can append, but not concurrently
            return 1;
    }
    // Cannot append at all; need ftrucnate().
    return 0;
}

inline
shared_ptr<file_impl>
make_file_impl(int fd, file_open_options options) {
    struct stat st;
    auto r = ::fstat(fd, &st);
    throw_system_error_on(r == -1);

    r = ::ioctl(fd, BLKGETSIZE);
    io_queue& io_queue = engine().get_io_queue(st.st_dev);

    // FIXME: obtain these flags from somewhere else
    auto flags = ::fcntl(fd, F_GETFL);
    throw_system_error_on(flags == -1);

    if (r != -1) {
        return make_shared<blockdev_file_impl>(fd, open_flags(flags), options, &io_queue);
    } else {
        if ((flags & O_ACCMODE) == O_RDONLY) {
            return make_shared<posix_file_impl>(fd, open_flags(flags), options, &io_queue);
        }
        if (S_ISDIR(st.st_mode)) {
            return make_shared<posix_file_impl>(fd, open_flags(flags), options, &io_queue);
        }
        struct append_support {
            bool append_challenged;
            unsigned append_concurrency;
            bool fsync_is_exclusive;
        };
        static thread_local std::unordered_map<decltype(st.st_dev), append_support> s_fstype;
        if (!s_fstype.count(st.st_dev)) {
            struct statfs sfs;
            auto r = ::fstatfs(fd, &sfs);
            throw_system_error_on(r == -1);
            append_support as;
            switch (sfs.f_type) {
            case 0x58465342: /* XFS */
                as.append_challenged = true;
                static auto xc = xfs_concurrency_from_kernel_version();
                as.append_concurrency = xc;
                as.fsync_is_exclusive = true;
                break;
            case 0x6969: /* NFS */
                as.append_challenged = false;
                as.append_concurrency = 0;
                as.fsync_is_exclusive = false;
                break;
            case 0xEF53: /* EXT4 */
                as.append_challenged = true;
                as.append_concurrency = 0;
                as.fsync_is_exclusive = false;
                break;
            default:
                as.append_challenged = true;
                as.append_concurrency = 0;
                as.fsync_is_exclusive = true;
            }
            s_fstype[st.st_dev] = as;
        }
        auto as = s_fstype[st.st_dev];
        if (!as.append_challenged) {
            return make_shared<posix_file_impl>(fd, open_flags(flags), options, &io_queue);
        }
        return make_shared<append_challenged_posix_file_impl>(fd, open_flags(flags), options, as.append_concurrency, as.fsync_is_exclusive, &io_queue);
    }
}

file::file(int fd, file_open_options options)
        : _file_impl(make_file_impl(fd, options)) {
}

file::file(seastar::file_handle&& handle)
        : _file_impl(std::move(std::move(handle).to_file()._file_impl)) {
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

}
