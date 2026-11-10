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
#include <iostream>
#include <seastar/core/alien.hh>
#include <seastar/core/smp.hh>
#include <seastar/core/app-template.hh>
#include <seastar/core/posix.hh>
#include <seastar/core/reactor.hh>
#include <seastar/util/later.hh>
#include <exception>
#include <stdexcept>
#include <ranges>
#include <tuple>
#include <string_view>
#include <memory>
#include <chrono>
#include <type_traits>
#include <seastar/core/sleep.hh>
#include <seastar/util/defer.hh>


using namespace seastar;

struct empty_result {};

struct ref_qualified_callback {
    future<int> operator()() & {
        return make_ready_future<int>(17);
    }
    future<int> operator()() && {
        return make_ready_future<int>(29);
    }
};

struct category_dependent_result_callback {
    future<int> operator()() & {
        return make_ready_future<int>(53);
    }
    future<long> operator()() && {
        return make_ready_future<long>(99);
    }
};

struct lvalue_only_callback {
    future<int> operator()() & {
        return make_ready_future<int>(61);
    }
};

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
    seastar::app_template app;

    unsigned shard_count = 0;

    // use the raw fd, because seastar engine want to take over the fds, if it
    // polls on them.
    // The eventfd protocol requires this thread to run concurrently.
    auto zim = std::async(std::launch::async, [&app, &shard_count, engine_ready_fd,
                           alien_done=alien_done.get()] {
        // Notify the reactor even when a new assertion throws: otherwise the
        // test deadlocks instead of reporting the failed assertion.
        [[maybe_unused]] auto notify_done = defer([alien_done] () noexcept {
            (void)::eventfd_write(alien_done, ALIEN_DONE);
        });
        eventfd_t result = 0;
        // wait until the seastar engine is ready
        int r = ::eventfd_read(engine_ready_fd, &result);
        if (r < 0) {
            throw std::runtime_error("failed to wait for seastar engine");
        }
        if (result != ENGINE_READY) {
            throw std::runtime_error("seastar failed to sent us the ready message");
        }
        // test for alien::run_on()
        std::promise<char> question;
        auto answer = question.get_future();
        alien::run_on(app.alien(), 0, [&question]() noexcept {
            question.set_value('*');
        });
        // test for alien::submit_to(), which returns a std::future<int>
        std::vector<std::future<int>> counts;
        for (auto i : std::views::iota(0u, shard_count)) {
            // send messages from alien.
            counts.push_back(alien::submit_to(app.alien(), i, [i] {
                return seastar::make_ready_future<int>(i);
            }));
        }
        // test for alien::submit_to(), which returns a std::future<void>
        alien::submit_to(app.alien(), 0, [] {
            return seastar::make_ready_future<>();
        }).get();

        // An immediate return value (or void) must also be futurized.
        if (alien::submit_to(app.alien(), 0, [] { return 42; }).get() != 42) {
            throw std::runtime_error("alien::submit_to lost immediate return value");
        }
        alien::submit_to(app.alien(), 0, [] {}).get();

        // Empty user-defined objects must not be confused with Seastar's
        // internal monostate used for void returns.
        auto immediate_empty = alien::submit_to(app.alien(), 0, [] { return empty_result{}; });
        static_assert(std::is_same_v<decltype(immediate_empty), std::future<empty_result>>);
        (void)immediate_empty.get();
        auto future_empty = alien::submit_to(app.alien(), 0, [] {
            return make_ready_future<empty_result>(empty_result{});
        });
        static_assert(std::is_same_v<decltype(future_empty), std::future<empty_result>>);
        (void)future_empty.get();

        // Move-only return values and genuinely asynchronous futures.
        auto move_only = alien::submit_to(app.alien(), 0, [] {
            return std::make_unique<int>(43);
        }).get();
        if (!move_only || *move_only != 43) {
            throw std::runtime_error("alien::submit_to lost move-only return value");
        }
        auto delayed = alien::submit_to(app.alien(), 0, [] {
            return sleep(std::chrono::milliseconds(1)).then([] { return 73; });
        });
        if (delayed.get() != 73) {
            throw std::runtime_error("alien::submit_to lost delayed return value");
        }

        // A synchronous exception must reach the alien caller, not terminate
        // the reactor through the noexcept run_on() callback.
        auto synchronous_failure = alien::submit_to(app.alien(), 0, [] () -> future<int> {
            throw std::runtime_error("alien synchronous failure");
        });
        try {
            (void)synchronous_failure.get();
            throw std::logic_error("alien::submit_to swallowed a synchronous exception");
        } catch (const std::runtime_error& ex) {
            if (std::string_view(ex.what()) != "alien synchronous failure") {
                throw;
            }
        }

        // The callable itself can be move-only, not just its result.
        auto move_capture = alien::submit_to(app.alien(), 0,
                [value = std::make_unique<int>(44)] () mutable {
            return std::move(value);
        });
        auto captured_result = move_capture.get();
        if (!captured_result || *captured_result != 44) {
            throw std::runtime_error("alien move-only callback lost its value");
        }

        // An exception produced after an asynchronous wait must also cross
        // the alien-thread boundary without being lost.
        auto delayed_failure = alien::submit_to(app.alien(), 0, [] {
            return sleep(std::chrono::milliseconds(1)).then([] () -> int {
                throw std::runtime_error("alien delayed failure");
            });
        });
        try {
            (void)delayed_failure.get();
            throw std::logic_error("alien::submit_to swallowed a delayed failure");
        } catch (const std::runtime_error& ex) {
            if (std::string_view(ex.what()) != "alien delayed failure") {
                throw;
            }
        }

        // Invoke the stored callback as an lvalue, as the original code did.
        auto qualified = alien::submit_to(app.alien(), 0, ref_qualified_callback{});
        if (qualified.get() != 17) {
            throw std::runtime_error("alien reference-qualified callback changed invocation category");
        }

        // Deduce the result from the same lvalue invocation, not operator()&&.
        auto category_result = alien::submit_to(app.alien(), 0, category_dependent_result_callback{});
        static_assert(std::is_same_v<decltype(category_result), std::future<int>>);
        if (category_result.get() != 53) {
            throw std::runtime_error("alien category-dependent result was not preserved");
        }

        // A callable invocable only on an lvalue is valid after capture.
        auto lvalue_only = alien::submit_to(app.alien(), 0, lvalue_only_callback{});
        if (lvalue_only.get() != 61) {
            throw std::runtime_error("alien lvalue-only callback result was not preserved");
        }

        // A failed Seastar future must still propagate correctly.
        auto asynchronous_failure = alien::submit_to(app.alien(), 0, [] {
            return make_exception_future<int>(std::runtime_error("alien asynchronous failure"));
        });
        try {
            (void)asynchronous_failure.get();
            throw std::logic_error("alien::submit_to swallowed a failed future");
        } catch (const std::runtime_error& ex) {
            if (std::string_view(ex.what()) != "alien asynchronous failure") {
                throw;
            }
        }

        int total = 0;
        for (auto& count : counts) {
            total += count.get();
        }
        // notify_done dismisses the engine on normal and exceptional exit.
        return std::make_tuple(answer.get(), total);
    });

    eventfd_t result = 0;
    app.run(argc, argv, [&] {
        return seastar::now().then([engine_ready_fd, &shard_count] {
            // engine ready!
            shard_count = this_smp_shard_count();
            ::eventfd_write(engine_ready_fd, ENGINE_READY);
            return seastar::now();
        }).then([alien_done = std::move(alien_done), &result]() mutable {
            return do_with(seastar::pollable_fd(std::move(alien_done)), [&result] (pollable_fd& alien_done_fds) {
                // check if alien has dismissed me.
                return alien_done_fds.readable().then([&result, &alien_done_fds] {
                    auto ret = alien_done_fds.get_file_desc().read(&result, sizeof(result));
                    return make_ready_future<size_t>(*ret);
                });
            });
        }).then([&result](size_t n) {
            if (n != sizeof(result)) {
                throw std::runtime_error("read from eventfd failed");
            }
            if (result != ALIEN_DONE) {
                throw std::logic_error("alien failed to dismiss me");
            }
            return seastar::now();
        }).handle_exception([](auto ep) {
            std::cerr << fmt::format("Error: {}\n", seastar::formattable(ep));
        }).finally([] {
            seastar::engine().exit(0);
        });
    });
    std::tuple<char, int> alien_result;
    try {
        alien_result = zim.get();
    } catch (const std::exception& ex) {
        std::cerr << "Alien thread failed: " << ex.what() << '\n';
        return 1;
    }
    auto [everything, total] = alien_result;
    if (char expected = '*'; everything != '*') {
        std::cerr << "Bad everything: " << everything << " != " << expected << std::endl;
        return 1;
    }
    const auto shards = std::views::iota(0u, shard_count);
    auto expected = std::accumulate(std::begin(shards), std::end(shards), 0);
    if (total != expected) {
        std::cerr << "Bad total: " << total << " != " << expected << std::endl;
        return 1;
    }
}
