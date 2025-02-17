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
 * Copyright (C) 2014-2015 Cloudius Systems, Ltd.
 */

#include <filesystem>

#include <seastar/testing/random.hh>
#include <seastar/testing/test_case.hh>
#include <seastar/testing/thread_test_case.hh>
#include <seastar/testing/test_runner.hh>

#include <seastar/core/reactor.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/semaphore.hh>
#include <seastar/core/condition-variable.hh>
#include <seastar/core/file.hh>
#include <seastar/core/layered_file.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/stall_sampler.hh>
#include <seastar/core/aligned_buffer.hh>
#include <seastar/core/io_intent.hh>
#include <seastar/util/assert.hh>
#include <seastar/util/tmp_file.hh>
#include <seastar/util/alloc_failure_injector.hh>
#include <seastar/util/closeable.hh>
#include <seastar/util/internal/magic.hh>
#include <seastar/util/internal/iovec_utils.hh>

#include <boost/range/adaptor/transformed.hpp>
#include <iostream>
#include <sys/statfs.h>
#include <fcntl.h>

using namespace seastar;
namespace fs = std::filesystem;

SEASTAR_TEST_CASE(open_flags_test) {
    open_flags flags = open_flags::rw | open_flags::create  | open_flags::exclusive;
    BOOST_REQUIRE(std::underlying_type_t<open_flags>(flags) ==
                  (std::underlying_type_t<open_flags>(open_flags::rw) |
                   std::underlying_type_t<open_flags>(open_flags::create) |
                   std::underlying_type_t<open_flags>(open_flags::exclusive)));

    open_flags mask = open_flags::create  | open_flags::exclusive;
    BOOST_REQUIRE((flags & mask) == mask);
    return make_ready_future<>();
}

SEASTAR_TEST_CASE(access_flags_test) {
    access_flags flags = access_flags::read | access_flags::write  | access_flags::execute;
    BOOST_REQUIRE(std::underlying_type_t<open_flags>(flags) ==
                  (std::underlying_type_t<open_flags>(access_flags::read) |
                   std::underlying_type_t<open_flags>(access_flags::write) |
                   std::underlying_type_t<open_flags>(access_flags::execute)));
    return make_ready_future<>();
}

SEASTAR_TEST_CASE(file_exists_test) {
    return tmp_dir::do_with_thread([] (tmp_dir& t) {
        sstring filename = (t.get_path() / "testfile.tmp").native();
        auto f = open_file_dma(filename, open_flags::rw | open_flags::create).get();
        f.close().get();
        auto exists = file_exists(filename).get();
        BOOST_REQUIRE(exists);
        remove_file(filename).get();
        exists = file_exists(filename).get();
        BOOST_REQUIRE(!exists);
    });
}

SEASTAR_TEST_CASE(handle_bad_alloc_test) {
    return tmp_dir::do_with_thread([] (tmp_dir& t) {
        sstring filename = (t.get_path() / "testfile.tmp").native();
        auto f = open_file_dma(filename, open_flags::rw | open_flags::create).get();
        f.close().get();
        bool exists = false;
        memory::with_allocation_failures([&] {
            exists = file_exists(filename).get();
        });
        BOOST_REQUIRE(exists);
    });
}

SEASTAR_TEST_CASE(file_access_test) {
    return tmp_dir::do_with_thread([] (tmp_dir& t) {
        sstring filename = (t.get_path() / "testfile.tmp").native();
        auto f = open_file_dma(filename, open_flags::rw | open_flags::create).get();
        f.close().get();
        auto is_accessible = file_accessible(filename, access_flags::read | access_flags::write).get();
        BOOST_REQUIRE(is_accessible);
    });
}

struct file_test {
    file_test(file&& f) : f(std::move(f)) {}
    file f;
    semaphore sem = { 0 };
    semaphore par = { 1000 };
};

SEASTAR_TEST_CASE(test1) {
    // Note: this tests generates a file "testfile.tmp" with size 4096 * max (= 40 MB).
  return tmp_dir::do_with([] (tmp_dir& t) {
    static constexpr auto max = 10000;
    sstring filename = (t.get_path() / "testfile.tmp").native();
    return open_file_dma(filename, open_flags::rw | open_flags::create).then([filename] (file f) {
        auto ft = new file_test{std::move(f)};
        for (size_t i = 0; i < max; ++i) {
            // Don't wait for future, use semaphore to signal when done instead.
            (void)ft->par.wait().then([ft, i] {
                auto wbuf = allocate_aligned_buffer<unsigned char>(4096, 4096);
                std::fill(wbuf.get(), wbuf.get() + 4096, i);
                auto wb = wbuf.get();
                (void)ft->f.dma_write(i * 4096, wb, 4096).then(
                        [ft, i, wbuf = std::move(wbuf)] (size_t ret) mutable {
                    BOOST_REQUIRE(ret == 4096);
                    auto rbuf = allocate_aligned_buffer<unsigned char>(4096, 4096);
                    auto rb = rbuf.get();
                    (void)ft->f.dma_read(i * 4096, rb, 4096).then(
                            [ft, rbuf = std::move(rbuf), wbuf = std::move(wbuf)] (size_t ret) mutable {
                        BOOST_REQUIRE(ret == 4096);
                        BOOST_REQUIRE(std::equal(rbuf.get(), rbuf.get() + 4096, wbuf.get()));
                        ft->sem.signal(1);
                        ft->par.signal();
                    });
                });
            });
        }
        return ft->sem.wait(max).then([ft] () mutable {
            return ft->f.flush();
        }).then([ft] {
            return ft->f.close();
        }).then([ft] () mutable {
            delete ft;
        });
    });
  });
}

SEASTAR_TEST_CASE(parallel_write_fsync) {
    return internal::report_reactor_stalls([] {
        return tmp_dir::do_with_thread([] (tmp_dir& t) {
            // Plan: open a file and write to it like crazy. In parallel fsync() it all the time.
            auto fname = (t.get_path() / "testfile.tmp").native();
            auto sz = uint64_t(32*1024*1024);
            auto buffer_size = 32768;
            auto write_concurrency = 16;
            auto fsync_every = 1024*1024;
            auto max_write_ahead_of_fsync = 4*1024*1024; // ensures writes don't complete too quickly
            auto written = uint64_t(0);
            auto fsynced_at = uint64_t(0);

            file f = open_file_dma(fname, open_flags::rw | open_flags::create | open_flags::truncate).get();
            auto close_f = deferred_close(f);
            // Avoid filesystem problems with size-extending operations
            f.truncate(sz).get();

            auto fsync_semaphore = semaphore(0);
            auto may_write_condvar = condition_variable();
            auto fsync_thread = thread([&] {
                auto fsynced = uint64_t(0);
                while (fsynced < sz) {
                    fsync_semaphore.wait(fsync_every).get();
                    fsynced_at = written;
                    // Signal the condition variable now so that writes proceed
                    // in parallel with the fsync
                    may_write_condvar.broadcast();
                    f.flush().get();
                    fsynced += fsync_every;
                }
            });

            auto write_semaphore = semaphore(write_concurrency);
            while (written < sz) {
                write_semaphore.wait().get();
                may_write_condvar.wait([&] {
                    return written <= fsynced_at + max_write_ahead_of_fsync;
                }).get();
                auto buf = temporary_buffer<char>::aligned(f.memory_dma_alignment(), buffer_size);
                memset(buf.get_write(), 0, buf.size());
                // Write asynchronously, signal when done.
                (void)f.dma_write(written, buf.get(), buf.size()).then([&fsync_semaphore, &write_semaphore, buf = std::move(buf)] (size_t w) {
                    fsync_semaphore.signal(buf.size());
                    write_semaphore.signal();
                });
                written += buffer_size;
            }
            write_semaphore.wait(write_concurrency).get();

            fsync_thread.join().get();
            close_f.close_now();
            remove_file(fname).get();
        });
    }).then([] (internal::stall_report sr) {
        std::cout << "parallel_write_fsync: " << sr << "\n";
    });
}

SEASTAR_TEST_CASE(test_iov_max) {
  return tmp_dir::do_with_thread([] (tmp_dir& t) {
    static constexpr size_t buffer_size = 4096;
    static constexpr size_t buffer_count = IOV_MAX * 2 + 1;

    std::vector<temporary_buffer<char>> original_buffers;
    std::vector<iovec> iovecs;
    for (auto i = 0u; i < buffer_count; i++) {
        original_buffers.emplace_back(temporary_buffer<char>::aligned(buffer_size, buffer_size));
        std::fill_n(original_buffers.back().get_write(), buffer_size, char(i));
        iovecs.emplace_back(iovec { original_buffers.back().get_write(), buffer_size });
    }

    auto filename = (t.get_path() / "testfile.tmp").native();
    auto f = open_file_dma(filename, open_flags::rw | open_flags::create).get();
    auto close_f = deferred_close(f);
    size_t left = buffer_size * buffer_count;
    size_t position = 0;
    while (left) {
        auto written = f.dma_write(position, iovecs).get();
        iovecs.erase(iovecs.begin(), iovecs.begin() + written / buffer_size);
        SEASTAR_ASSERT(written % buffer_size == 0);
        position += written;
        left -= written;
    }

    BOOST_CHECK(iovecs.empty());

    std::vector<temporary_buffer<char>> read_buffers;
    for (auto i = 0u; i < buffer_count; i++) {
        read_buffers.emplace_back(temporary_buffer<char>::aligned(buffer_size, buffer_size));
        std::fill_n(read_buffers.back().get_write(), buffer_size, char(0));
        iovecs.emplace_back(iovec { read_buffers.back().get_write(), buffer_size });
    }

    left = buffer_size * buffer_count;
    position = 0;
    while (left) {
        auto read = f.dma_read(position, iovecs).get();
        iovecs.erase(iovecs.begin(), iovecs.begin() + read / buffer_size);
        SEASTAR_ASSERT(read % buffer_size == 0);
        position += read;
        left -= read;
    }

    for (auto i = 0u; i < buffer_count; i++) {
        BOOST_CHECK(std::equal(original_buffers[i].get(), original_buffers[i].get() + original_buffers[i].size(),
                               read_buffers[i].get(), read_buffers[i].get() + read_buffers[i].size()));
    }
  });
}

SEASTAR_THREAD_TEST_CASE(test_sanitize_iovecs) {
    auto buf = temporary_buffer<char>::aligned(4096, 4096);

    auto iovec_equal = [] (const iovec& a, const iovec& b) {
        return a.iov_base == b.iov_base && a.iov_len == b.iov_len;
    };

    { // Single fragment, sanitize is noop
        auto original_iovecs = std::vector<iovec> { { buf.get_write(), buf.size() } };
        auto actual_iovecs = original_iovecs;
        auto actual_length = internal::sanitize_iovecs(actual_iovecs, 4096);
        BOOST_CHECK_EQUAL(actual_length, 4096);
        BOOST_CHECK_EQUAL(actual_iovecs.size(), 1);
        BOOST_CHECK(iovec_equal(original_iovecs.back(), actual_iovecs.back()));
    }

    { // one 1024 buffer and IOV_MAX+6 buffers of 512; 4096 byte disk alignment, sanitize needs to drop buffers
        auto original_iovecs = std::vector<iovec>{};
        for (auto i = 0u; i < IOV_MAX + 7; i++) {
            original_iovecs.emplace_back(iovec { buf.get_write(), i == 0 ? 1024u : 512u });
        }
        auto actual_iovecs = original_iovecs;
        auto actual_length = internal::sanitize_iovecs(actual_iovecs, 4096);
        BOOST_CHECK_EQUAL(actual_length, 512 * IOV_MAX);
        BOOST_CHECK_EQUAL(actual_iovecs.size(), IOV_MAX - 1);

        original_iovecs.resize(IOV_MAX - 1);
        BOOST_CHECK(std::equal(original_iovecs.begin(), original_iovecs.end(),
                               actual_iovecs.begin(), actual_iovecs.end(), iovec_equal));
    }

    { // IOV_MAX-1 buffers of 512, one 1024 buffer, and 6 512 buffers; 4096 byte disk alignment, sanitize needs to drop and trim buffers
        auto original_iovecs = std::vector<iovec>{};
        for (auto i = 0u; i < IOV_MAX + 7; i++) {
            original_iovecs.emplace_back(iovec { buf.get_write(), i == (IOV_MAX - 1) ? 1024u : 512u });
        }
        auto actual_iovecs = original_iovecs;
        auto actual_length = internal::sanitize_iovecs(actual_iovecs, 4096);
        BOOST_CHECK_EQUAL(actual_length, 512 * IOV_MAX);
        BOOST_CHECK_EQUAL(actual_iovecs.size(), IOV_MAX);

        original_iovecs.resize(IOV_MAX);
        original_iovecs.back().iov_len = 512;
        BOOST_CHECK(std::equal(original_iovecs.begin(), original_iovecs.end(),
                               actual_iovecs.begin(), actual_iovecs.end(), iovec_equal));
    }

    { // IOV_MAX+8 buffers of 512; 4096 byte disk alignment, sanitize needs to drop buffers
        auto original_iovecs = std::vector<iovec>{};
        for (auto i = 0u; i < IOV_MAX + 8; i++) {
            original_iovecs.emplace_back(iovec { buf.get_write(), 512 });
        }
        auto actual_iovecs = original_iovecs;
        auto actual_length = internal::sanitize_iovecs(actual_iovecs, 4096);
        BOOST_CHECK_EQUAL(actual_length, 512 * IOV_MAX);
        BOOST_CHECK_EQUAL(actual_iovecs.size(), IOV_MAX);

        original_iovecs.resize(IOV_MAX);
        BOOST_CHECK(std::equal(original_iovecs.begin(), original_iovecs.end(),
                               actual_iovecs.begin(), actual_iovecs.end(), iovec_equal));
    }
}

SEASTAR_TEST_CASE(test_chmod) {
  return tmp_dir::do_with_thread([] (tmp_dir& t) {
    auto oflags = open_flags::rw | open_flags::create;
    sstring filename = (t.get_path() / "testfile.tmp").native();
    if (file_exists(filename).get()) {
        remove_file(filename).get();
    }

    auto orig_umask = umask(0);

    // test default_file_permissions
    auto f = open_file_dma(filename, oflags).get();
    f.close().get();
    auto sd = file_stat(filename).get();
    BOOST_CHECK_EQUAL(sd.mode & static_cast<mode_t>(file_permissions::all_permissions), static_cast<mode_t>(file_permissions::default_file_permissions));

    // test chmod with new_permissions
    auto new_permissions = file_permissions::user_read | file_permissions::group_read | file_permissions::others_read;
    BOOST_REQUIRE(new_permissions != file_permissions::default_file_permissions);
    BOOST_REQUIRE(file_exists(filename).get());
    chmod(filename, new_permissions).get();
    sd = file_stat(filename).get();
    BOOST_CHECK_EQUAL(sd.mode & static_cast<mode_t>(file_permissions::all_permissions), static_cast<mode_t>(new_permissions));
    remove_file(filename).get();

    umask(orig_umask);
  });
}

SEASTAR_TEST_CASE(test_open_file_dma_permissions) {
  return tmp_dir::do_with_thread([] (tmp_dir& t) {
    auto oflags = open_flags::rw | open_flags::create;
    sstring filename = (t.get_path() / "testfile.tmp").native();
    if (file_exists(filename).get()) {
        remove_file(filename).get();
    }

    auto orig_umask = umask(0);

    // test default_file_permissions
    auto f = open_file_dma(filename, oflags).get();
    f.close().get();
    auto sd = file_stat(filename).get();
    BOOST_CHECK_EQUAL(sd.mode & static_cast<mode_t>(file_permissions::all_permissions), static_cast<mode_t>(file_permissions::default_file_permissions));
    remove_file(filename).get();

    // test options.create_permissions
    auto options = file_open_options();
    options.create_permissions = file_permissions::user_read | file_permissions::group_read | file_permissions::others_read;
    BOOST_REQUIRE(options.create_permissions != file_permissions::default_file_permissions);
    f = open_file_dma(filename, oflags, options).get();
    f.close().get();
    sd = file_stat(filename).get();
    BOOST_CHECK_EQUAL(sd.mode & static_cast<mode_t>(file_permissions::all_permissions), static_cast<mode_t>(options.create_permissions));
    remove_file(filename).get();

    umask(orig_umask);
  });
}

SEASTAR_TEST_CASE(test_make_directory_permissions) {
  return tmp_dir::do_with_thread([] (tmp_dir& t) {
    sstring dirname = (t.get_path() / "testdir.tmp").native();
    auto orig_umask = umask(0);

    // test default_dir_permissions with make_directory
    make_directory(dirname).get();
    auto sd = file_stat(dirname).get();
    BOOST_CHECK_EQUAL(sd.mode & static_cast<mode_t>(file_permissions::all_permissions), static_cast<mode_t>(file_permissions::default_dir_permissions));
    remove_file(dirname).get();

    // test make_directory
    auto create_permissions = file_permissions::user_read | file_permissions::group_read | file_permissions::others_read;
    BOOST_REQUIRE(create_permissions != file_permissions::default_dir_permissions);
    make_directory(dirname, create_permissions).get();
    sd = file_stat(dirname).get();
    BOOST_CHECK_EQUAL(sd.mode & static_cast<mode_t>(file_permissions::all_permissions), static_cast<mode_t>(create_permissions));
    remove_file(dirname).get();

    umask(orig_umask);
  });
}

SEASTAR_TEST_CASE(test_touch_directory_permissions) {
  return tmp_dir::do_with_thread([] (tmp_dir& t) {
    sstring dirname = (t.get_path() / "testdir.tmp").native();
    auto orig_umask = umask(0);

    // test default_dir_permissions with touch_directory
    touch_directory(dirname).get();
    auto sd = file_stat(dirname).get();
    BOOST_CHECK_EQUAL(sd.mode & static_cast<mode_t>(file_permissions::all_permissions), static_cast<mode_t>(file_permissions::default_dir_permissions));
    remove_file(dirname).get();

    // test touch_directory, dir creation
    auto create_permissions = file_permissions::user_read | file_permissions::group_read | file_permissions::others_read;
    BOOST_REQUIRE(create_permissions != file_permissions::default_dir_permissions);
    BOOST_REQUIRE(!file_exists(dirname).get());
    touch_directory(dirname, create_permissions).get();
    sd = file_stat(dirname).get();
    BOOST_CHECK_EQUAL(sd.mode & static_cast<mode_t>(file_permissions::all_permissions), static_cast<mode_t>(create_permissions));

    // test touch_directory of existing dir, dir mode need not change
    BOOST_REQUIRE(file_exists(dirname).get());
    touch_directory(dirname, file_permissions::default_dir_permissions).get();
    sd = file_stat(dirname).get();
    BOOST_CHECK_EQUAL(sd.mode & static_cast<mode_t>(file_permissions::all_permissions), static_cast<mode_t>(create_permissions));
    remove_file(dirname).get();

    umask(orig_umask);
  });
}

SEASTAR_TEST_CASE(test_recursive_touch_directory_permissions) {
  return tmp_dir::do_with_thread([] (tmp_dir& t) {
    sstring base_dirname = (t.get_path() / "testbasedir.tmp").native();
    sstring dirpath = base_dirname + "/" + "testsubdir.tmp";
    if (file_exists(dirpath).get()) {
        remove_file(dirpath).get();
    }
    if (file_exists(base_dirname).get()) {
        remove_file(base_dirname).get();
    }

    auto orig_umask = umask(0);

    // test default_dir_permissions with recursive_touch_directory
    recursive_touch_directory(dirpath).get();
    auto sd = file_stat(base_dirname).get();
    BOOST_CHECK_EQUAL(sd.mode & static_cast<mode_t>(file_permissions::all_permissions), static_cast<mode_t>(file_permissions::default_dir_permissions));
    sd = file_stat(dirpath).get();
    BOOST_CHECK_EQUAL(sd.mode & static_cast<mode_t>(file_permissions::all_permissions), static_cast<mode_t>(file_permissions::default_dir_permissions));
    remove_file(dirpath).get();

    // test recursive_touch_directory, dir creation
    auto create_permissions = file_permissions::user_read | file_permissions::group_read | file_permissions::others_read;
    BOOST_REQUIRE(create_permissions != file_permissions::default_dir_permissions);
    BOOST_REQUIRE(file_exists(base_dirname).get());
    BOOST_REQUIRE(!file_exists(dirpath).get());
    recursive_touch_directory(dirpath, create_permissions).get();
    sd = file_stat(base_dirname).get();
    BOOST_CHECK_EQUAL(sd.mode & static_cast<mode_t>(file_permissions::all_permissions), static_cast<mode_t>(file_permissions::default_dir_permissions));
    sd = file_stat(dirpath).get();
    BOOST_CHECK_EQUAL(sd.mode & static_cast<mode_t>(file_permissions::all_permissions), static_cast<mode_t>(create_permissions));

    // test recursive_touch_directory of existing dir, dir mode need not change
    BOOST_REQUIRE(file_exists(dirpath).get());
    recursive_touch_directory(dirpath, file_permissions::default_dir_permissions).get();
    sd = file_stat(base_dirname).get();
    BOOST_CHECK_EQUAL(sd.mode & static_cast<mode_t>(file_permissions::all_permissions), static_cast<mode_t>(file_permissions::default_dir_permissions));
    sd = file_stat(dirpath).get();
    BOOST_CHECK_EQUAL(sd.mode & static_cast<mode_t>(file_permissions::all_permissions), static_cast<mode_t>(create_permissions));
    remove_file(dirpath).get();
    remove_file(base_dirname).get();

    umask(orig_umask);
  });
}

SEASTAR_TEST_CASE(test_file_stat_method) {
  return tmp_dir::do_with_thread([] (tmp_dir& t) {
    auto oflags = open_flags::rw | open_flags::create;
    sstring filename = (t.get_path() / "testfile.tmp").native();

    auto orig_umask = umask(0);

    auto f = open_file_dma(filename, oflags).get();
    auto close_f = deferred_close(f);
    auto st = f.stat().get();
    BOOST_CHECK_EQUAL(st.st_mode & static_cast<mode_t>(file_permissions::all_permissions), static_cast<mode_t>(file_permissions::default_file_permissions));

    umask(orig_umask);
  });
}

SEASTAR_TEST_CASE(test_file_write_lifetime_method) {
    return tmp_dir::do_with_thread([] (tmp_dir& t) {
        auto oflags = open_flags::rw | open_flags::create;
        sstring filename = (t.get_path() / "testfile.tmp").native();

        auto f1 = open_file_dma(filename, oflags).get();
        auto close_f1 = deferred_close(f1);
        auto f2 = open_file_dma(filename, oflags).get();
        auto close_f2 = deferred_close(f2);

        // Write life time hint values
        std::vector<uint64_t> hint_set = {RWF_WRITE_LIFE_NOT_SET,
                                                RWH_WRITE_LIFE_NONE,
                                                RWH_WRITE_LIFE_SHORT,
                                                RWH_WRITE_LIFE_MEDIUM,
                                                RWH_WRITE_LIFE_LONG,
                                                RWH_WRITE_LIFE_EXTREME};

        for (auto i = 0ul; i < hint_set.size(); ++i) {
            auto hint = hint_set[i];

            // Set and verify the lifetime hint of the inode
            f1.set_inode_lifetime_hint(hint).get();
            auto o_hint1 = f1.get_inode_lifetime_hint().get();
            BOOST_CHECK_EQUAL(hint, o_hint1);
        }

        // Perform invalid ops
        uint64_t hint = RWH_WRITE_LIFE_EXTREME + 1;
        BOOST_REQUIRE_THROW(f1.set_inode_lifetime_hint(hint).get(), std::system_error);
    });
}

SEASTAR_TEST_CASE(test_file_fcntl) {
    return tmp_dir::do_with_thread([] (tmp_dir& t) {
        auto oflags = open_flags::rw | open_flags::create;
        sstring filename = (t.get_path() / "testfile.tmp").native();

        auto f = open_file_dma(filename, oflags).get();
        auto close_f = deferred_close(f);

        // Set and verify a lease value
        auto lease = F_WRLCK;
        BOOST_REQUIRE(!f.fcntl(F_SETLEASE, lease).get());
        auto o_lease = f.fcntl(F_GETLEASE).get();
        BOOST_CHECK_EQUAL(lease, o_lease);

        // Use _short version and test the same
        o_lease = f.fcntl_short(F_GETLEASE).get();
        BOOST_CHECK_EQUAL(lease, o_lease);

        // Perform invalid ops
        BOOST_REQUIRE_THROW(f.fcntl(F_SETLEASE, (uintptr_t)~0ul).get(), std::system_error);
        BOOST_REQUIRE_THROW(f.fcntl_short(F_SETLEASE, (uintptr_t)~0ul).get(), std::system_error);
    });
}

SEASTAR_TEST_CASE(test_file_ioctl) {
    return tmp_dir::do_with_thread([] (tmp_dir& t) {
        auto oflags = open_flags::rw | open_flags::create;
        sstring filename = (t.get_path() / "testfile.tmp").native();
        uint64_t block_size = 0;

        auto f = open_file_dma(filename, oflags).get();
        auto close_f = deferred_close(f);

        // Issueing an FS ioctl which is applicable on regular files
        // and can be executed as normal user
        try {
            BOOST_REQUIRE(!f.ioctl(FIGETBSZ, &block_size).get());
            BOOST_REQUIRE(block_size != 0);

            // Use _short version and test the same
            BOOST_REQUIRE(!f.ioctl_short(FIGETBSZ, &block_size).get());
            BOOST_REQUIRE(block_size != 0);
        } catch (std::system_error& e) {
            // anon_bdev filesystems do not support FIGETBSZ, and return EINVAL
            BOOST_REQUIRE_EQUAL(e.code().value(), EINVAL);
        }

        // Perform invalid ops
        BOOST_REQUIRE_THROW(f.ioctl(FIGETBSZ, 0ul).get(), std::system_error);
        BOOST_REQUIRE_THROW(f.ioctl_short(FIGETBSZ, 0ul).get(), std::system_error);
    });
}

class test_layered_file : public layered_file_impl {
public:
    explicit test_layered_file(file f) : layered_file_impl(std::move(f)) {}
    virtual future<size_t> write_dma(uint64_t pos, const void* buffer, size_t len, io_intent*) override {
        abort();
    }
    virtual future<size_t> write_dma(uint64_t pos, std::vector<iovec> iov, io_intent*) override {
        abort();
    }
    virtual future<size_t> read_dma(uint64_t pos, void* buffer, size_t len, io_intent*) override {
        abort();
    }
    virtual future<size_t> read_dma(uint64_t pos, std::vector<iovec> iov, io_intent*) override {
        abort();
    }
    virtual future<> flush(void) override {
        abort();
    }
    virtual future<struct stat> stat(void) override {
        abort();
    }
    virtual future<> truncate(uint64_t length) override {
        abort();
    }
    virtual future<> discard(uint64_t offset, uint64_t length) override {
        abort();
    }
    virtual future<> allocate(uint64_t position, uint64_t length) override {
        abort();
    }
    virtual future<uint64_t> size(void) override {
        abort();
    }
    virtual future<> close() override {
        abort();
    }
    virtual std::unique_ptr<file_handle_impl> dup() override {
        abort();
    }
    virtual subscription<directory_entry> list_directory(std::function<future<> (directory_entry de)> next) override {
        abort();
    }
    virtual future<temporary_buffer<uint8_t>> dma_read_bulk(uint64_t offset, size_t range_size, io_intent*) override {
        abort();
    }
};

SEASTAR_TEST_CASE(test_underlying_file) {
    return tmp_dir::do_with_thread([] (tmp_dir& t) {
        auto oflags = open_flags::rw | open_flags::create;
        sstring filename = (t.get_path() / "testfile.tmp").native();
        auto f = open_file_dma(filename, oflags).get();
        auto close_f = deferred_close(f);
        auto lf = file(make_shared<test_layered_file>(f));
        BOOST_CHECK_EQUAL(f.memory_dma_alignment(), lf.memory_dma_alignment());
        BOOST_CHECK_EQUAL(f.disk_read_dma_alignment(), lf.disk_read_dma_alignment());
        BOOST_CHECK_EQUAL(f.disk_write_dma_alignment(), lf.disk_write_dma_alignment());
    });
}

SEASTAR_TEST_CASE(test_file_stat_method_with_file) {
    return tmp_dir::do_with_thread([] (tmp_dir& t) {
        auto oflags = open_flags::rw | open_flags::create | open_flags::truncate;
        sstring filename = (t.get_path() / "testfile.tmp").native();
        file ref;

        auto orig_umask = umask(0);

        auto st = with_file(open_file_dma(filename, oflags), [&ref] (file& f) {
            // make a copy of f to verify f is auto-closed when `with_file` returns.
            ref = f;
            return f.stat();
        }).get();
        BOOST_CHECK_EQUAL(st.st_mode & static_cast<mode_t>(file_permissions::all_permissions), static_cast<mode_t>(file_permissions::default_file_permissions));

        // verify that the file was auto-closed
        BOOST_REQUIRE_THROW(ref.stat().get(), std::system_error);

        umask(orig_umask);
    });
}

SEASTAR_TEST_CASE(test_open_error_with_file) {
    return tmp_dir::do_with_thread([] (tmp_dir& t) {
        auto open_file = [&t] (bool do_open) {
            auto oflags = open_flags::ro;
            sstring filename = (t.get_path() / "testfile.tmp").native();
            if (do_open) {
                return open_file_dma(filename, oflags);
            } else {
                throw std::runtime_error("expected exception");
            }
        };
        bool got_exception = false;

        BOOST_REQUIRE_NO_THROW(with_file(open_file(true), [] (file& f) {
                BOOST_REQUIRE(false);
            }).handle_exception_type([&got_exception] (const std::system_error& e) {
                got_exception = true;
                BOOST_REQUIRE(e.code().value() == ENOENT);
            }).get());
        BOOST_REQUIRE(got_exception);

        got_exception = false;
        BOOST_REQUIRE_THROW(with_file(open_file(false), [] (file& f) {
                BOOST_REQUIRE(false);
            }).handle_exception_type([&got_exception] (const std::runtime_error& e) {
                got_exception = true;
            }).get(), std::runtime_error);
        BOOST_REQUIRE(!got_exception);
    });
}

SEASTAR_TEST_CASE(test_with_file_close_on_failure) {
    return tmp_dir::do_with_thread([] (tmp_dir& t) {
        auto oflags = open_flags::rw | open_flags::create | open_flags::truncate;
        sstring filename = (t.get_path() / "testfile.tmp").native();

        auto orig_umask = umask(0);

        // error-free case
        auto ref = with_file_close_on_failure(open_file_dma(filename, oflags), [] (file& f) {
            return f;
        }).get();
        auto st = ref.stat().get();
        ref.close().get();
        BOOST_CHECK_EQUAL(st.st_mode & static_cast<mode_t>(file_permissions::all_permissions), static_cast<mode_t>(file_permissions::default_file_permissions));

        // close-on-error case
        BOOST_REQUIRE_THROW(with_file_close_on_failure(open_file_dma(filename, oflags), [&ref] (file& f) {
            ref = f;
            throw std::runtime_error("expected exception");
        }).get(), std::runtime_error);

        // verify that file was auto-closed on error
        BOOST_REQUIRE_THROW(ref.stat().get(), std::system_error);

        umask(orig_umask);
    });
}

namespace seastar {
    extern bool aio_nowait_supported;
}

SEASTAR_TEST_CASE(test_nowait_flag_correctness) {
    return tmp_dir::do_with_thread([] (tmp_dir& t) {
        auto oflags = open_flags::rw | open_flags::create;
        sstring filename = (t.get_path() / "testfile.tmp").native();
        auto is_tmpfs = [&] (sstring filename) {
            struct ::statfs buf;
            int fd = ::open(filename.c_str(), static_cast<int>(open_flags::ro));
            SEASTAR_ASSERT(fd != -1);
            auto r = ::fstatfs(fd, &buf);
            if (r == -1) {
                return false;
            }
            return buf.f_type == internal::fs_magic::tmpfs;
        };

        if (!seastar::aio_nowait_supported) {
            BOOST_TEST_WARN(0, "Skipping this test because RWF_NOWAIT is not supported by the system");
            return;
        }

        auto f = open_file_dma(filename, oflags).get();
        auto close_f = deferred_close(f);

        if (is_tmpfs(filename)) {
            BOOST_TEST_WARN(0, "Skipping this test because TMPFS was detected, and RWF_NOWAIT is only supported by disk-based FSes");
            return;
        }

        for (auto i = 0; i < 10; i++) {
            auto wbuf = allocate_aligned_buffer<unsigned char>(4096, 4096);
            std::fill(wbuf.get(), wbuf.get() + 4096, i);
            auto wb = wbuf.get();
            f.dma_write(i * 4096, wb, 4096).get();
            f.flush().get();
        }
    });
}

SEASTAR_TEST_CASE(test_destruct_just_constructed_append_challenged_file) {
    return tmp_dir::do_with_thread([] (tmp_dir& t) {
        sstring filename = (t.get_path() / "testfile.tmp").native();
        auto oflags = open_flags::rw | open_flags::create;
        auto f = open_file_dma(filename, oflags).get();
    });
}

SEASTAR_TEST_CASE(test_destruct_just_constructed_append_challenged_file_with_sloppy_size) {
    return tmp_dir::do_with_thread([] (tmp_dir& t) {
        sstring filename = (t.get_path() / "testfile.tmp").native();
        auto oflags = open_flags::rw | open_flags::create;
        file_open_options opt;
        opt.sloppy_size = true;
        auto f = open_file_dma(filename, oflags, opt).get();
    });
}

SEASTAR_TEST_CASE(test_destruct_append_challenged_file_after_write) {
    return tmp_dir::do_with_thread([] (tmp_dir& t) {
        sstring filename = (t.get_path() / "testfile.tmp").native();
        auto buf = allocate_aligned_buffer<unsigned char>(4096, 4096);
        std::fill(buf.get(), buf.get() + 4096, 0);

        auto f = open_file_dma(filename, open_flags::rw | open_flags::create).get();
        f.dma_write(0, buf.get(), 4096).get();
    });
}

SEASTAR_TEST_CASE(test_destruct_append_challenged_file_after_read) {
    return tmp_dir::do_with_thread([] (tmp_dir& t) {
        sstring filename = (t.get_path() / "testfile.tmp").native();
        auto buf = allocate_aligned_buffer<unsigned char>(4096, 4096);
        std::fill(buf.get(), buf.get() + 4096, 0);

        auto f = open_file_dma(filename, open_flags::rw | open_flags::create).get();
        f.dma_write(0, buf.get(), 4096).get();
        f.flush().get();
        f.close().get();

        f = open_file_dma(filename, open_flags::rw).get();
        f.dma_read(0, buf.get(), 4096).get();
    });
}

SEASTAR_TEST_CASE(test_dma_iovec) {
    return tmp_dir::do_with_thread([] (tmp_dir& t) {
        static constexpr size_t alignment = 4096;
        auto wbuf = allocate_aligned_buffer<char>(alignment, alignment);
        size_t size = 1234;
        std::fill_n(wbuf.get(), alignment, char(0));
        std::fill_n(wbuf.get(), size, char(42));
        std::vector<iovec> iovecs;

        auto filename = (t.get_path() / "testfile.tmp").native();
        auto f = open_file_dma(filename, open_flags::rw | open_flags::create).get();
        iovecs.push_back(iovec{ wbuf.get(), alignment });
        auto count = f.dma_write(0, iovecs).get();
        BOOST_REQUIRE_EQUAL(count, alignment);
        f.truncate(size).get();
        f.close().get();

        auto rbuf = allocate_aligned_buffer<char>(alignment, alignment);

        // this tests the posix_file_impl
        f = open_file_dma(filename, open_flags::ro).get();
        std::fill_n(rbuf.get(), alignment, char(0));
        iovecs.clear();
        iovecs.push_back(iovec{ rbuf.get(), alignment });
        count = f.dma_read(0, iovecs).get();
        BOOST_REQUIRE_EQUAL(count, size);

        BOOST_REQUIRE(std::equal(wbuf.get(), wbuf.get() + alignment, rbuf.get(), rbuf.get() + alignment));

        // this tests the append_challenged_posix_file_impl
        f = open_file_dma(filename, open_flags::rw).get();
        std::fill_n(rbuf.get(), alignment, char(0));
        iovecs.clear();
        iovecs.push_back(iovec{ rbuf.get(), alignment });
        count = f.dma_read(0, iovecs).get();
        BOOST_REQUIRE_EQUAL(count, size);

        BOOST_REQUIRE(std::equal(wbuf.get(), wbuf.get() + alignment, rbuf.get(), rbuf.get() + alignment));
    });
}

SEASTAR_TEST_CASE(test_intent) {
    return tmp_dir::do_with_thread([] (tmp_dir& t) {
        sstring filename = (t.get_path() / "testfile.tmp").native();
        auto f = open_file_dma(filename, open_flags::rw | open_flags::create).get();
        auto buf = allocate_aligned_buffer<unsigned char>(1024, 1024);
        std::fill(buf.get(), buf.get() + 1024, 'a');
        f.dma_write(0, buf.get(), 1024).get();
        std::fill(buf.get(), buf.get() + 1024, 'b');
        io_intent intent;
        auto f1 = f.dma_write(0, buf.get(), 512);
        auto f2 = f.dma_write(512, buf.get(), 512, &intent);
        intent.cancel();

        bool cancelled = false;
        f1.get();
        try {
            f2.get();
        } catch (cancelled_error& ex) {
            cancelled = true;
        }
        auto rbuf = allocate_aligned_buffer<unsigned char>(1024, 1024);
        f.dma_read(0, rbuf.get(), 1024).get();
        BOOST_REQUIRE(rbuf.get()[0] == 'b');
        if (cancelled) {
            BOOST_REQUIRE(rbuf.get()[512] == 'a');
        } else {
            // The file::dma_write doesn't preemt, but if it
            // suddenly will, the 2nd write will pass before
            // the intent would be cancelled
            BOOST_TEST_WARN(0, "Write won the race with cancellation");
            BOOST_REQUIRE(rbuf.get()[512] == 'b');
        }
    });
}

SEASTAR_TEST_CASE(parallel_overwrite) {
    // Avoid /tmp for tmp_dir, since it can be tmpfs
    return tmp_dir::do_with("XXXXXXXX.tmp", [] (tmp_dir& t) {
        return async([&] {
            // Check that overwrites at disk_overwrite_dma_alignment() do not cause stalls. First,
            // create a file.
            auto fname = (t.get_path() / "testfile.tmp").native();
            auto sz = uint64_t(1*1024*1024);
            auto buffer_size = 128*1024;

            file f = open_file_dma(fname, open_flags::rw | open_flags::create | open_flags::truncate).get();
            // Avoid filesystem problems with size-extending operations
            f.truncate(sz).get();
            auto buf = allocate_aligned_buffer<unsigned char>(buffer_size, f.memory_dma_alignment());
            for (uint64_t offset = 0; offset < sz; offset += buffer_size) {
                f.dma_write(offset, buf.get(), buffer_size).get();
            }

            auto random_engine = testing::local_random_engine;
            auto dist = std::uniform_int_distribution(uint64_t(0), sz-1);
            auto offsets  = std::vector<uint64_t>();
            std::generate_n(std::back_insert_iterator(offsets), 5000, [&] { return align_down(dist(random_engine), f.disk_overwrite_dma_alignment()); });
            auto stall_report = internal::report_reactor_stalls([&] {
                return max_concurrent_for_each(offsets, 10, [&] (uint64_t offset) {
                    return f.dma_write(offset, buf.get(), f.disk_overwrite_dma_alignment()).discard_result();
                });
            }).get();
            std::cout << "parallel_overwrite: " << stall_report << " (overwrite dma alignment " << f.disk_overwrite_dma_alignment() << ")\n";

            f.close().get();
            remove_file(fname).get();
        });
    });
}

SEASTAR_TEST_CASE(test_oversized_io_works) {
    return tmp_dir::do_with_thread([] (tmp_dir& t) {
        sstring filename = (t.get_path() / "testfile.tmp").native();
        auto f = open_file_dma(filename, open_flags::rw | open_flags::create).get();

        size_t max_write = f.disk_write_max_length();
        size_t max_read = f.disk_read_max_length();
        size_t buf_size = std::max(max_write, max_read) + 4096;

        auto buf = allocate_aligned_buffer<unsigned char>(buf_size, 4096);
        std::fill(buf.get(), buf.get() + buf_size, 'a');

        f.dma_write(0, buf.get(), buf_size).get();
        f.flush().get();
        f.close().get();

        std::fill(buf.get(), buf.get() + buf_size, 'b');
        f = open_file_dma(filename, open_flags::rw).get();
        f.dma_read(0, buf.get(), buf_size).get();

        BOOST_REQUIRE((size_t)std::count_if(buf.get(), buf.get() + buf_size, [](auto x) { return x == 'a'; }) == buf_size);
    });
}

SEASTAR_TEST_CASE(test_file_system_space) {
    return tmp_dir::do_with_thread([] (tmp_dir& t) {
        const auto& name = t.get_path().native();
        auto st = engine().statvfs(name).get();
        auto si = file_system_space(name).get();

        BOOST_REQUIRE_EQUAL(st.f_blocks * st.f_frsize, si.capacity);
        BOOST_REQUIRE_LT(si.free, si.capacity);
        BOOST_REQUIRE_LT(si.available, si.capacity);
        BOOST_REQUIRE_LE(si.available, si.free);
    });
}
