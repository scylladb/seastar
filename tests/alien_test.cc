// -*- mode:C++; tab-width:4; c-basic-offset:4; indent-tabs-mode:nil -*-
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
 * Copyright (C) 2018 Red Hat
 */

#include <future>
#include <numeric>
#include "core/alien.hh"
#include "core/app-template.hh"
#include "core/posix.hh"
#include "core/reactor.hh"

using namespace seastar;

enum {
    ENGINE_READY = 24,
    ALIEN_DONE   = 42,
};

int main(int argc, char** argv)
{
    // we need a protocol that both seastar and alien understand.
    // and on which, a seastar future can wait.
    int engine_ready_fd = eventfd(0, 0);
    auto alien_done = file_desc::eventfd(0, 0);

    // use the raw fd, because seastar engine want to take over the fds, if it
    // polls on them.
    auto zim = std::async([engine_ready_fd,
                           alien_done=alien_done.get()] {
        eventfd_t result = 0;
        // wait until the seastar engine is ready
        int r = ::eventfd_read(engine_ready_fd, &result);
        if (r < 0) {
            return -EINVAL;
        }
        if (result != ENGINE_READY) {
            return -EINVAL;
        }
        std::vector<std::future<int>> counts;
        for (auto i : boost::irange(0u, smp::count)) {
            // send messages from alien.
            counts.push_back(alien::submit_to(i, [i] {
                return seastar::make_ready_future<int>(i);
            }));
        }
        int total = 0;
        for (auto& count : counts) {
            total += count.get();
        }
        // i am done. dismiss the engine
        ::eventfd_write(alien_done, ALIEN_DONE);
        return total;
    });

    seastar::app_template app;
    seastar::pollable_fd_state alien_done_fds{std::move(alien_done)};
    eventfd_t result = 0;
    app.run(argc, argv, [&] {
        return seastar::now().then([engine_ready_fd] {
            // engine ready!
            ::eventfd_write(engine_ready_fd, ENGINE_READY);
            return seastar::now();
        }).then([&alien_done_fds, &result]() {
            // check if alien has dismissed me.
            return seastar::engine().read_some(alien_done_fds, &result, sizeof(result));
        }).then([&result](size_t n) {
            if (n != sizeof(result)) {
                throw std::runtime_error("read from eventfd failed");
            }
            if (result != ALIEN_DONE) {
                throw std::logic_error("alien failed to dismiss me");
            }
            return seastar::now();
        }).handle_exception([](auto ep) {
            std::cerr << "Error: " << ep << std::endl;
        }).finally([] {
            seastar::engine().exit(0);
        });
    });
    int total = zim.get();
    const auto shards = boost::irange(0u, smp::count);
    auto expected = std::accumulate(std::begin(shards), std::end(shards), 0);
    if (total != expected) {
        std::cerr << "Bad total: " << total << " != " << expected << std::endl;
        return 1;
    }
}
