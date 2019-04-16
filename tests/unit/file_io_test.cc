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

#include <seastar/testing/test_case.hh>
#include <seastar/testing/thread_test_case.hh>

#include <seastar/core/semaphore.hh>
#include <seastar/core/condition-variable.hh>
#include <seastar/core/file.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/stall_sampler.hh>
#include <boost/range/adaptor/transformed.hpp>
#include <iostream>

#include "core/file-impl.hh"

using namespace seastar;

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
    return seastar::async([] {
        sstring filename = "testfile.tmp";
        auto f = open_file_dma(filename, open_flags::rw | open_flags::create).get0();
        f.close().get();
        auto exists = file_exists(filename).get0();
        BOOST_REQUIRE(exists);
        remove_file(filename).get();
        exists = file_exists(filename).get0();
        BOOST_REQUIRE(!exists);
    });
}

SEASTAR_TEST_CASE(file_access_test) {
    return seastar::async([] {
        sstring filename = "testfile.tmp";
        auto f = open_file_dma(filename, open_flags::rw | open_flags::create).get0();
        f.close().get();
        auto is_accessible = file_accessible(filename, access_flags::read | access_flags::write).get0();
        BOOST_REQUIRE(is_accessible);
        remove_file(filename).get();
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
    static constexpr auto max = 10000;
    return open_file_dma("testfile.tmp", open_flags::rw | open_flags::create).then([] (file f) {
        auto ft = new file_test{std::move(f)};
        for (size_t i = 0; i < max; ++i) {
            ft->par.wait().then([ft, i] {
                auto wbuf = allocate_aligned_buffer<unsigned char>(4096, 4096);
                std::fill(wbuf.get(), wbuf.get() + 4096, i);
                auto wb = wbuf.get();
                ft->f.dma_write(i * 4096, wb, 4096).then(
                        [ft, i, wbuf = std::move(wbuf)] (size_t ret) mutable {
                    BOOST_REQUIRE(ret == 4096);
                    auto rbuf = allocate_aligned_buffer<unsigned char>(4096, 4096);
                    auto rb = rbuf.get();
                    ft->f.dma_read(i * 4096, rb, 4096).then(
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
            std::cout << "done\n";
            delete ft;
        });
    });
}

SEASTAR_TEST_CASE(parallel_write_fsync) {
    return internal::report_reactor_stalls([] {
        return async([] {
            // Plan: open a file and write to it like crazy. In parallel fsync() it all the time.
            auto fname = "testfile.tmp";
            auto sz = uint64_t(32*1024*1024);
            auto buffer_size = 32768;
            auto write_concurrency = 16;
            auto fsync_every = 1024*1024;
            auto max_write_ahead_of_fsync = 4*1024*1024; // ensures writes don't complete too quickly
            auto written = uint64_t(0);
            auto fsynced_at = uint64_t(0);

            file f = open_file_dma(fname, open_flags::rw | open_flags::create | open_flags::truncate).get0();
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
                f.dma_write(written, buf.get(), buf.size()).then([&fsync_semaphore, &write_semaphore, buf = std::move(buf)] (size_t w) {
                    fsync_semaphore.signal(buf.size());
                    write_semaphore.signal();
                });
                written += buffer_size;
            }
            write_semaphore.wait(write_concurrency).get();

            fsync_thread.join().get();
            f.close().get();
            remove_file(fname).get();
        });
    }).then([] (internal::stall_report sr) {
        std::cout << "parallel_write_fsync: " << sr << "\n";
    });
}

SEASTAR_THREAD_TEST_CASE(test_iov_max) {
    static constexpr size_t buffer_size = 4096;
    static constexpr size_t buffer_count = IOV_MAX * 2 + 1;

    std::vector<temporary_buffer<char>> original_buffers;
    std::vector<iovec> iovecs;
    for (auto i = 0u; i < buffer_count; i++) {
        original_buffers.emplace_back(temporary_buffer<char>::aligned(buffer_size, buffer_size));
        std::fill_n(original_buffers.back().get_write(), buffer_size, char(i));
        iovecs.emplace_back(iovec { original_buffers.back().get_write(), buffer_size });
    }

    auto f = open_file_dma("testfile.tmp", open_flags::rw | open_flags::create).get0();
    size_t left = buffer_size * buffer_count;
    size_t position = 0;
    while (left) {
        auto written = f.dma_write(position, iovecs).get0();
        iovecs.erase(iovecs.begin(), iovecs.begin() + written / buffer_size);
        assert(written % buffer_size == 0);
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
        auto read = f.dma_read(position, iovecs).get0();
        iovecs.erase(iovecs.begin(), iovecs.begin() + read / buffer_size);
        assert(read % buffer_size == 0);
        position += read;
        left -= read;
    }

    for (auto i = 0u; i < buffer_count; i++) {
        BOOST_CHECK(std::equal(original_buffers[i].get(), original_buffers[i].get() + original_buffers[i].size(),
                               read_buffers[i].get(), read_buffers[i].get() + read_buffers[i].size()));
    }

    f.close().get();
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

SEASTAR_THREAD_TEST_CASE(test_chmod) {
    auto oflags = open_flags::rw | open_flags::create;
    sstring filename = "testfile.tmp";
    if (file_exists(filename).get0()) {
        remove_file(filename).get();
    }

    auto orig_umask = umask(0);

    // test default_file_permissions
    auto f = open_file_dma(filename, oflags).get0();
    f.close().get();
    auto sd = file_stat(filename).get0();
    BOOST_CHECK_EQUAL(sd.mode & static_cast<mode_t>(file_permissions::all_permissions), static_cast<mode_t>(file_permissions::default_file_permissions));

    // test chmod with new_permissions
    auto new_permissions = file_permissions::user_read | file_permissions::group_read | file_permissions::others_read;
    BOOST_REQUIRE(new_permissions != file_permissions::default_file_permissions);
    BOOST_REQUIRE(file_exists(filename).get0());
    chmod(filename, new_permissions).get();
    sd = file_stat(filename).get0();
    BOOST_CHECK_EQUAL(sd.mode & static_cast<mode_t>(file_permissions::all_permissions), static_cast<mode_t>(new_permissions));
    remove_file(filename).get();

    umask(orig_umask);
}

SEASTAR_THREAD_TEST_CASE(test_open_file_dma_permissions) {
    auto oflags = open_flags::rw | open_flags::create;
    sstring filename = "testfile.tmp";
    if (file_exists(filename).get0()) {
        remove_file(filename).get();
    }

    auto orig_umask = umask(0);

    // test default_file_permissions
    auto f = open_file_dma(filename, oflags).get0();
    f.close().get();
    auto sd = file_stat(filename).get0();
    BOOST_CHECK_EQUAL(sd.mode & static_cast<mode_t>(file_permissions::all_permissions), static_cast<mode_t>(file_permissions::default_file_permissions));
    remove_file(filename).get();

    // test options.create_permissions
    auto options = file_open_options();
    options.create_permissions = file_permissions::user_read | file_permissions::group_read | file_permissions::others_read;
    BOOST_REQUIRE(options.create_permissions != file_permissions::default_file_permissions);
    f = open_file_dma(filename, oflags, options).get0();
    f.close().get();
    sd = file_stat(filename).get0();
    BOOST_CHECK_EQUAL(sd.mode & static_cast<mode_t>(file_permissions::all_permissions), static_cast<mode_t>(options.create_permissions));
    remove_file(filename).get();

    umask(orig_umask);
}

SEASTAR_THREAD_TEST_CASE(test_make_directory_permissions) {
    sstring dirname = "testdir.tmp";
    if (file_exists(dirname).get0()) {
        remove_file(dirname).get();
    }

    auto orig_umask = umask(0);

    // test default_dir_permissions with make_directory
    make_directory(dirname).get();
    auto sd = file_stat(dirname).get0();
    BOOST_CHECK_EQUAL(sd.mode & static_cast<mode_t>(file_permissions::all_permissions), static_cast<mode_t>(file_permissions::default_dir_permissions));
    remove_file(dirname).get();

    // test make_directory
    auto create_permissions = file_permissions::user_read | file_permissions::group_read | file_permissions::others_read;
    BOOST_REQUIRE(create_permissions != file_permissions::default_dir_permissions);
    make_directory(dirname, create_permissions).get();
    sd = file_stat(dirname).get0();
    BOOST_CHECK_EQUAL(sd.mode & static_cast<mode_t>(file_permissions::all_permissions), static_cast<mode_t>(create_permissions));
    remove_file(dirname).get();

    umask(orig_umask);
}

SEASTAR_THREAD_TEST_CASE(test_touch_directory_permissions) {
    sstring dirname = "testdir.tmp";
    if (file_exists(dirname).get0()) {
        remove_file(dirname).get();
    }

    auto orig_umask = umask(0);

    // test default_dir_permissions with touch_directory
    touch_directory(dirname).get();
    auto sd = file_stat(dirname).get0();
    BOOST_CHECK_EQUAL(sd.mode & static_cast<mode_t>(file_permissions::all_permissions), static_cast<mode_t>(file_permissions::default_dir_permissions));
    remove_file(dirname).get();

    // test touch_directory, dir creation
    auto create_permissions = file_permissions::user_read | file_permissions::group_read | file_permissions::others_read;
    BOOST_REQUIRE(create_permissions != file_permissions::default_dir_permissions);
    BOOST_REQUIRE(!file_exists(dirname).get0());
    touch_directory(dirname, create_permissions).get();
    sd = file_stat(dirname).get0();
    BOOST_CHECK_EQUAL(sd.mode & static_cast<mode_t>(file_permissions::all_permissions), static_cast<mode_t>(create_permissions));

    // test touch_directory of existing dir, dir mode need not change
    BOOST_REQUIRE(file_exists(dirname).get0());
    touch_directory(dirname, file_permissions::default_dir_permissions).get();
    sd = file_stat(dirname).get0();
    BOOST_CHECK_EQUAL(sd.mode & static_cast<mode_t>(file_permissions::all_permissions), static_cast<mode_t>(create_permissions));
    remove_file(dirname).get();

    umask(orig_umask);
}

SEASTAR_THREAD_TEST_CASE(test_recursive_touch_directory_permissions) {
    sstring base_dirname = "testbasedir.tmp";
    sstring sub_dirname = "testsubdir.tmp";
    sstring dirpath = base_dirname + "/" + sub_dirname;
    if (file_exists(dirpath).get0()) {
        remove_file(dirpath).get();
    }
    if (file_exists(base_dirname).get0()) {
        remove_file(base_dirname).get();
    }

    auto orig_umask = umask(0);

    // test default_dir_permissions with recursive_touch_directory
    recursive_touch_directory(dirpath).get();
    auto sd = file_stat(base_dirname).get0();
    BOOST_CHECK_EQUAL(sd.mode & static_cast<mode_t>(file_permissions::all_permissions), static_cast<mode_t>(file_permissions::default_dir_permissions));
    sd = file_stat(dirpath).get0();
    BOOST_CHECK_EQUAL(sd.mode & static_cast<mode_t>(file_permissions::all_permissions), static_cast<mode_t>(file_permissions::default_dir_permissions));
    remove_file(dirpath).get();

    // test recursive_touch_directory, dir creation
    auto create_permissions = file_permissions::user_read | file_permissions::group_read | file_permissions::others_read;
    BOOST_REQUIRE(create_permissions != file_permissions::default_dir_permissions);
    BOOST_REQUIRE(file_exists(base_dirname).get0());
    BOOST_REQUIRE(!file_exists(dirpath).get0());
    recursive_touch_directory(dirpath, create_permissions).get();
    sd = file_stat(base_dirname).get0();
    BOOST_CHECK_EQUAL(sd.mode & static_cast<mode_t>(file_permissions::all_permissions), static_cast<mode_t>(file_permissions::default_dir_permissions));
    sd = file_stat(dirpath).get0();
    BOOST_CHECK_EQUAL(sd.mode & static_cast<mode_t>(file_permissions::all_permissions), static_cast<mode_t>(create_permissions));

    // test recursive_touch_directory of existing dir, dir mode need not change
    BOOST_REQUIRE(file_exists(dirpath).get0());
    recursive_touch_directory(dirpath, file_permissions::default_dir_permissions).get();
    sd = file_stat(base_dirname).get0();
    BOOST_CHECK_EQUAL(sd.mode & static_cast<mode_t>(file_permissions::all_permissions), static_cast<mode_t>(file_permissions::default_dir_permissions));
    sd = file_stat(dirpath).get0();
    BOOST_CHECK_EQUAL(sd.mode & static_cast<mode_t>(file_permissions::all_permissions), static_cast<mode_t>(create_permissions));
    remove_file(dirpath).get();
    remove_file(base_dirname).get();

    umask(orig_umask);
}
