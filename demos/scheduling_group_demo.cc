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
 * Copyright (C) 2016 Scylla DB Ltd
 */


#include <seastar/core/app-template.hh>
#include <seastar/core/future.hh>
#include <seastar/core/scheduling.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/loop.hh>
#include <seastar/core/when_all.hh>
#include <seastar/core/with_scheduling_group.hh>
#include <seastar/core/condition-variable.hh>
#include <seastar/util/defer.hh>
#include <fmt/printf.h>
#include <chrono>
#include <cmath>
#include <ranges>

using namespace seastar;
using namespace std::chrono_literals;

template <typename Func, typename Duration>
future<>
compute_intensive_task(Duration duration, unsigned& counter, Func func) {
    auto end = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < end) {
        func();
    }
    ++counter;
    return make_ready_future<>();
}

future<>
heavy_task(unsigned& counter) {
    return compute_intensive_task(1ms, counter, [] {
        static thread_local double x = 1;
        x = std::exp(x) / 3;
    });
}

future<>
light_task(unsigned& counter) {
    return compute_intensive_task(100us, counter, [] {
        static thread_local double x = 0.1;
        x = std::log(x + 1);
    });
}

future<>
medium_task(unsigned& counter) {
    return compute_intensive_task(400us, counter, [] {
        static thread_local double x = 0.1;
        x = std::cos(x);
    });
}

using done_func = std::function<bool ()>;

future<>
run_compute_intensive_tasks(seastar::scheduling_group sg, done_func done, unsigned concurrency, unsigned& counter, std::function<future<> (unsigned& counter)> task) {
    return seastar::async([task = std::move(task), sg, concurrency, done, &counter] () mutable {
        while (!done()) {
            parallel_for_each(std::views::iota(0u, concurrency), [task, sg, &counter] (unsigned i) mutable {
                return with_scheduling_group(sg, [task, &counter] {
                    return task(counter);
                });
            }).get();
            thread::maybe_yield();
        }
    });
}

future<>
run_compute_intensive_tasks_in_threads(seastar::scheduling_group sg, done_func done, unsigned concurrency, unsigned& counter, std::function<future<> (unsigned& counter)> task) {
    auto attr = seastar::thread_attributes();
    attr.sched_group = sg;
    return parallel_for_each(std::views::iota(0u, concurrency), [attr, done, &counter, task] (unsigned i) {
        return seastar::async(attr, [done, &counter, task] {
            while (!done()) {
                task(counter).get();
                thread::maybe_yield();
            }
        });
    });
}

future<>
run_with_duty_cycle(float utilization, std::chrono::steady_clock::duration period, done_func done, std::function<future<> (done_func done)> task) {
    return seastar::async([=] {
        bool duty_toggle = true;
        auto t0 = std::chrono::steady_clock::now();
        condition_variable cv;
        timer<> tmr_on([&] { duty_toggle = true; cv.signal(); });
        timer<> tmr_off([&] { duty_toggle = false; });
        tmr_on.arm(t0, period);
        tmr_off.arm(t0 + std::chrono::duration_cast<decltype(t0)::duration>(period * utilization), period);
        auto combined_done = [&] {
            return done() || !duty_toggle;
        };
        while (!done()) {
            while (!combined_done()) {
                task(std::cref(combined_done)).get();
                thread::maybe_yield();
            }
            cv.wait([&] {
                return done() || duty_toggle;
            }).get();
        }
        tmr_on.cancel();
        tmr_off.cancel();
    });
}

#include <fenv.h>

template <typename T>
auto var_fn(T& var) {
    return [&var] { return var; };
}

int main(int ac, char** av) {
    app_template app;
    return app.run(ac, av, [] {
        return seastar::async([] {
            auto sg100 = seastar::create_scheduling_group("sg100", 100).get();
            auto ksg100 = seastar::defer([&] () noexcept { seastar::destroy_scheduling_group(sg100).get(); });
            auto sg20 = seastar::create_scheduling_group("sg20", 20).get();
            auto ksg20 = seastar::defer([&] () noexcept { seastar::destroy_scheduling_group(sg20).get(); });
            auto sg50 = seastar::create_scheduling_group("sg50", 50).get();
            auto ksg50 = seastar::defer([&] () noexcept { seastar::destroy_scheduling_group(sg50).get(); });

            bool done = false;
            auto end = timer<>([&done] {
                done = true;
            });

            end.arm(10s);
            unsigned ctr100 = 0, ctr20 = 0, ctr50 = 0;
            fmt::print("running three scheduling groups with 100% duty cycle each:\n");
            when_all(
                    run_compute_intensive_tasks(sg100, var_fn(done), 5, ctr100, heavy_task),
                    run_compute_intensive_tasks(sg20, var_fn(done), 3, ctr20, light_task),
                    run_compute_intensive_tasks_in_threads(sg50, var_fn(done), 2, ctr50, medium_task)
                    ).get();
            fmt::print("{:10} {:15} {:10} {:12} {:8}\n", "shares", "task_time (us)", "executed", "runtime (ms)", "vruntime");
            fmt::print("{:10d} {:15d} {:10d} {:12d} {:8.2f}\n", 100, 1000, ctr100, ctr100 * 1000 / 1000, ctr100 * 1000 / 1000 / 100.);
            fmt::print("{:10d} {:15d} {:10d} {:12d} {:8.2f}\n", 20, 100, ctr20, ctr20 * 100 / 1000, ctr20 * 100 / 1000 / 20.);
            fmt::print("{:10d} {:15d} {:10d} {:12d} {:8.2f}\n", 50, 400, ctr50, ctr50 * 400 / 1000, ctr50 * 400 / 1000 / 50.);
            fmt::print("\n");

            fmt::print("running two scheduling groups with 100%/50% duty cycles (period=1s:\n");
            unsigned ctr100_2 = 0, ctr50_2 = 0;
            done = false;
            end.arm(10s);
            when_all(
                    run_compute_intensive_tasks(sg50, var_fn(done), 5, ctr50_2, heavy_task),
                    run_with_duty_cycle(0.5, 1s, var_fn(done), [=, &ctr100_2] (done_func done) {
                        return run_compute_intensive_tasks(sg100, done, 4, ctr100_2, heavy_task);
                    })
            ).get();
            fmt::print("{:10} {:10} {:15} {:10} {:12} {:8}\n", "shares", "duty", "task_time (us)", "executed", "runtime (ms)", "vruntime");
            fmt::print("{:10d} {:10d} {:15d} {:10d} {:12d} {:8.2f}\n", 100, 50, 1000, ctr100_2, ctr100_2 * 1000 / 1000, ctr100_2 * 1000 / 1000 / 100.);
            fmt::print("{:10d} {:10d} {:15d} {:10d} {:12d} {:8.2f}\n", 50, 100, 400, ctr50_2, ctr50_2 * 1000 / 1000, ctr50_2 * 1000 / 1000 / 50.);

            return 0;
        });
    });
}
