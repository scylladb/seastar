/*
 * Copyright 2016 ScyllaDB
 */
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

#include <seastar/core/posix.hh>
#include <seastar/util/assert.hh>
#include <seastar/util/defer.hh>
#include <seastar/core/linux-aio.hh>
#include <sys/time.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <iostream>
#include <unistd.h>
#include <cstdlib>
#include <type_traits>
#include <seastar/core/fsqual.hh>

namespace seastar {

using namespace seastar::internal;
using namespace seastar::internal::linux_abi;

// Runs func(), and also adds the number of context switches
// that happened during func() to counter.
template <typename Counter, typename Func>
typename std::invoke_result_t<Func>
with_ctxsw_counting(Counter& counter, Func&& func) {
    struct count_guard {
        Counter& counter;
        count_guard(Counter& counter) : counter(counter) {
            counter -= nvcsw();
        }
        ~count_guard() {
            counter += nvcsw();
        }
        static Counter nvcsw() {
            struct rusage usage;
            getrusage(RUSAGE_THREAD, &usage);
            return usage.ru_nvcsw;
        }
    };
    count_guard g(counter);
    return func();
}

bool filesystem_has_good_aio_support(sstring directory, bool verbose) {
    aio_context_t ioctx = {};
    auto r = io_setup(1, &ioctx);
    throw_system_error_on(r == -1, "io_setup");
    auto cleanup = defer([&] () noexcept { io_destroy(ioctx); });
    auto fname = directory + "/fsqual.tmp";
    auto fd = file_desc::open(fname, O_CREAT|O_EXCL|O_RDWR|O_DIRECT, 0600);
    unlink(fname.c_str());
    auto nr = 1000;
    fd.truncate(nr * 4096);
    auto bufsize = 4096;
    auto ctxsw = 0;
    auto buf = aligned_alloc(4096, 4096);
    auto del = defer([&] () noexcept { ::free(buf); });
    for (int i = 0; i < nr; ++i) {
        struct iocb cmd;
        cmd = make_write_iocb(fd.get(), bufsize*i, buf, bufsize);
        struct iocb* cmds[1] = { &cmd };
        with_ctxsw_counting(ctxsw, [&] {
            auto r = io_submit(ioctx, 1, cmds);
            throw_system_error_on(r == -1, "io_submit");
            SEASTAR_ASSERT(r == 1);
        });
        struct io_event ioev;
        int n = -1;
        do {
            n = io_getevents(ioctx, 1, 1, &ioev, nullptr);
            throw_system_error_on((n == -1) && (errno != EINTR) , "io_getevents");
        } while (n == -1);
        SEASTAR_ASSERT(n == 1);
        throw_kernel_error(long(ioev.res));
        SEASTAR_ASSERT(long(ioev.res) == bufsize);
    }
    auto rate = float(ctxsw) / nr;
    bool ok = rate < 0.1;
    if (verbose) {
        auto verdict = ok ? "GOOD" : "BAD";
        std::cout << "context switch per appending io: " << rate
                  << " (" << verdict << ")\n";
    }
    return ok;
}

}
