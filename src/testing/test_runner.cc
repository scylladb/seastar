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

#include <iostream>

#include <seastar/core/app-template.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/posix.hh>
#include <seastar/testing/random.hh>
#include <seastar/testing/test_runner.hh>
#include <seastar/util/assert.hh>

namespace seastar {

namespace testing {

static test_runner instance;

struct stop_execution : public std::exception {};

test_runner::~test_runner() {
    finalize();
}

bool
test_runner::start(int ac, char** av) {
    bool expected = false;
    if (!_started.compare_exchange_strong(expected, true, std::memory_order_acquire)) {
        return true;
    }

    // Don't interfere with seastar signal handling
    sigset_t mask;
    sigfillset(&mask);
    for (auto sig : { SIGSEGV }) {
        sigdelset(&mask, sig);
    }
    auto r = ::pthread_sigmask(SIG_BLOCK, &mask, NULL);
    if (r) {
        std::cerr << "Error blocking signals. Aborting." << std::endl;
        abort();
    }

    _st_args = std::make_unique<start_thread_args>(ac, av);
    return true;
}

bool test_runner::start_thread(int ac, char** av) {
    auto init_outcome = std::make_shared<exchanger<bool>>();

    namespace bpo = boost::program_options;
    _thread = std::make_unique<posix_thread>([this, ac, av, init_outcome]() mutable {
        app_template app;
        app.add_options()
            ("random-seed", bpo::value<unsigned>(), "Random number generator seed")
            ("fail-on-abandoned-failed-futures", bpo::value<bool>()->default_value(true), "Fail the test if there are any abandoned failed futures");
        // We guarantee that only one thread is running.
        // We only read this after that one thread is joined, so this is safe.
        _exit_code = app.run(ac, av, [this, &app, init_outcome = init_outcome.get()] {
            init_outcome->give(true);
            auto init = [&app] {
                auto conf_seed = app.configuration()["random-seed"];
                auto seed = conf_seed.empty() ? std::random_device()():  conf_seed.as<unsigned>();
                std::cout << "random-seed=" << seed << std::endl;
                return smp::invoke_on_all([seed] {
                    auto local_seed = seed + this_shard_id();
                    local_random_engine.seed(local_seed);
                });
            };

            return init().then([this] {
              return do_until([this] { return _done; }, [this] {
                // this will block the reactor briefly, but we don't care
                try {
                    auto func = _task.take();
                    return func();
                } catch (const stop_execution&) {
                    _done = true;
                    return make_ready_future<>();
                }
              }).or_terminate();
            }).then([&app] {
                if (engine().abandoned_failed_futures()) {
                    std::cerr << "*** " << engine().abandoned_failed_futures() << " abandoned failed future(s) detected" << std::endl;
                    if (app.configuration()["fail-on-abandoned-failed-futures"].as<bool>()) {
                        std::cerr << "Failing the test because fail was requested by --fail-on-abandoned-failed-futures" << std::endl;
                        return 3;
                    }
                }
                return 0;
            });
        });
        init_outcome->give(false);
    });

    return init_outcome->take();
}

void
test_runner::run_sync(std::function<future<>()> task) {
    if (_st_args) {
        start_thread_args sa = *_st_args;
        _st_args.reset();
        if (!start_thread(sa.ac, sa.av)) {
            // something bad happened when starting the reactor or app, and
            // the _thread has exited before taking any task. but we need to
            // move on. let's report this bad news with exit code
            _done = true;
        }
    }
    if (_done) {
        // we failed to start the worker reactor, so we cannot send the task to
        // it.
        return;
    }

    exchanger<std::exception_ptr> e;
    _task.give([task = std::move(task), &e] {
        SEASTAR_ASSERT(engine_is_ready());
        try {
            return task().then_wrapped([&e](auto&& f) {
                try {
                    f.get();
                    e.give({});
                } catch (...) {
                    e.give(std::current_exception());
                }
            });
        } catch (...) {
            e.give(std::current_exception());
            return make_ready_future<>();
        }
    });
    auto maybe_exception = e.take();
    if (maybe_exception) {
        std::rethrow_exception(maybe_exception);
    }
}

int test_runner::finalize() {
    if (_thread) {
        _task.interrupt(stop_execution());
        _thread->join();
        _thread = nullptr;
    }
    return _exit_code;
}

test_runner& global_test_runner() {
    return instance;
}

}

}
