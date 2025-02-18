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
 * Copyright (C) 2015 Cloudius Systems, Ltd.
 */

#pragma once

#include <memory>
#include <functional>
#include <atomic>
#include <seastar/core/future.hh>
#include <seastar/core/posix.hh>
#include <seastar/testing/exchanger.hh>

namespace seastar {

namespace testing {

class test_runner {
private:
    std::unique_ptr<posix_thread> _thread;
    std::atomic<bool> _started{false};
    exchanger<std::function<future<>()>> _task;
    bool _done = false;
    int _exit_code{0};

    struct start_thread_args {
        int ac;
        char** av;
        start_thread_args(int ac_, char** av_) noexcept : ac(ac_), av(av_) {}
    };
    std::unique_ptr<start_thread_args> _st_args;
    bool start_thread(int ac, char** av);
public:
    // Returns whether initialization was successful.
    // Will return as soon as the seastar::app was started.
    bool start(int argc, char** argv);
    ~test_runner();
    void run_sync(std::function<future<>()> task);
    // Returns the return value of the underlying `seastar::app::run()`.
    int finalize();
};

test_runner& global_test_runner();

}

}
