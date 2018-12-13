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

#include <seastar/core/semaphore.hh>
#include <seastar/core/condition-variable.hh>
#include <seastar/core/file.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/stall_sampler.hh>
#include <iostream>

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
