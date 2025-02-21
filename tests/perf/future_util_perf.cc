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
 * Copyright (C) 2018 ScyllaDB Ltd.
 */

#include <boost/range.hpp>
#include <boost/range/irange.hpp>

#include <seastar/testing/perf_tests.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/coroutine/as_future.hh>
#include <seastar/coroutine/parallel_for_each.hh>
#include <seastar/core/loop.hh>
#include <seastar/util/later.hh>

struct parallel_for_each {
    std::vector<int> empty_range;
    std::vector<int> range;
    int value;

    static constexpr int max_range_size = 100;

    parallel_for_each()
        : empty_range()
        , range(boost::copy_range<std::vector<int>>(boost::irange(1, max_range_size)))
    { }
};

PERF_TEST_F(parallel_for_each, empty)
{
    return seastar::parallel_for_each(empty_range, [] (int) -> future<> {
        abort();
    });
}

[[gnu::noinline]]
future<> immediate(int v, int& vs)
{
    vs += v;
    return make_ready_future<>();
}

struct chain {
    static constexpr int scale = 32;
    int value = 0;

    [[gnu::noinline]]
    future<> do_then(future<> w, int depth = scale) {
        if (depth > 0) {
            return do_then(std::move(w), depth - 1).then([this] {
                value += 1;
                perf_tests::do_not_optimize(value);
                return make_ready_future<>();
            });
        } else {
            return w;
        }
    }

    [[gnu::noinline]]
    future<> do_await(future<> w, int depth = scale) {
        if (depth > 0) {
            co_await do_await(std::move(w), depth - 1);
            value += 1;
            perf_tests::do_not_optimize(value);
        } else {
            co_await std::move(w);
        }
    }

    [[gnu::noinline]]
    future<std::nullopt_t> do_await_as_future(future<std::nullopt_t> w, int depth = scale) {
        if (depth > 0) {
            auto fut = co_await coroutine::as_future<std::nullopt_t>(do_await_as_future(std::move(w), depth - 1));
            if (fut.failed()) {
                co_await seastar::coroutine::return_exception_ptr(fut.get_exception());
            }
            value += 1;
            perf_tests::do_not_optimize(value);
            co_return fut.get();
        } else {
            auto fut = co_await coroutine::as_future<std::nullopt_t>(std::move(w));
            if (fut.failed()) {
                co_await seastar::coroutine::return_exception_ptr(fut.get_exception());
            }
            co_return fut.get();
        }
    }
};

PERF_TEST_F(chain, then_value)
{
    promise<> p;
    auto f = do_then(p.get_future());
    p.set_value();
    return f.then([] { return scale; });
}

PERF_TEST_F(chain, await_value)
{
    promise<> p;
    auto f = do_await(p.get_future());
    p.set_value();
    return f.then([] { return scale; });
}

PERF_TEST_F(chain, await_value_as_future)
{
    promise<std::nullopt_t> p;
    auto f = do_await_as_future(p.get_future());
    p.set_value(std::nullopt);
    return f.then([](std::nullopt_t) { return scale; });
}

PERF_TEST_F(chain, then_exception)
{
    promise<> p;
    auto f = do_then(p.get_future());
    p.set_exception(std::runtime_error(""));
    return f.then_wrapped([] (auto x) {
        x.ignore_ready_future();
        return scale;
    });
}

PERF_TEST_F(chain, await_exception)
{
    promise<> p;
    auto f = do_await(p.get_future());
    p.set_exception(std::runtime_error(""));
    return f.then_wrapped([] (auto x) {
        x.ignore_ready_future();
        return scale;
    });
}

PERF_TEST_F(chain, await_exception_as_future)
{
    promise<std::nullopt_t> p;
    auto f = do_await_as_future(p.get_future());
    p.set_exception(std::runtime_error(""));
    return f.then_wrapped([] (auto x) {
        x.ignore_ready_future();
        return scale;
    });
}

PERF_TEST_F(parallel_for_each, immediate_1)
{
    auto&& begin = range.begin();
    auto&& end = begin + 1;
    return seastar::parallel_for_each(std::move(begin), std::move(end), [this] (int v) {
        return immediate(v, value);
    }).then([this] {
        perf_tests::do_not_optimize(value);
        return 1;
    });
}

PERF_TEST_F(parallel_for_each, immediate_2)
{
    auto&& begin = range.begin();
    auto&& end = begin + 2;
    return seastar::parallel_for_each(std::move(begin), std::move(end), [this] (int v) {
        return immediate(v, value);
    }).then([this] {
        perf_tests::do_not_optimize(value);
        return 2;
    });
}

PERF_TEST_F(parallel_for_each, immediate_10)
{
    auto&& begin = range.begin();
    auto&& end = begin + 10;
    return seastar::parallel_for_each(std::move(begin), std::move(end), [this] (int v) {
        return immediate(v, value);
    }).then([this] {
        perf_tests::do_not_optimize(value);
        return 10;
    });
}

PERF_TEST_F(parallel_for_each, immediate_100)
{
    return seastar::parallel_for_each(range, [this] (int v) {
        return immediate(v, value);
    }).then([this] {
        perf_tests::do_not_optimize(value);
        return range.size();
    });
}

[[gnu::noinline]]
future<> suspend(int v, int& vs)
{
    vs += v;
    return yield();
}

PERF_TEST_F(parallel_for_each, suspend_1)
{
    auto&& begin = range.begin();
    auto&& end = begin + 1;
    return seastar::parallel_for_each(std::move(begin), std::move(end), [this] (int v) {
        return suspend(v, value);
    }).then([this] {
        perf_tests::do_not_optimize(value);
        return 1;
    });
}

PERF_TEST_F(parallel_for_each, suspend_2)
{
    auto&& begin = range.begin();
    auto&& end = begin + 2;
    return seastar::parallel_for_each(std::move(begin), std::move(end), [this] (int v) {
        return suspend(v, value);
    }).then([this] {
        perf_tests::do_not_optimize(value);
        return 2;
    });
}

PERF_TEST_F(parallel_for_each, suspend_10)
{
    auto&& begin = range.begin();
    auto&& end = begin + 10;
    return seastar::parallel_for_each(std::move(begin), std::move(end), [this] (int v) {
        return suspend(v, value);
    }).then([this] {
        perf_tests::do_not_optimize(value);
        return 10;
    });
}

PERF_TEST_F(parallel_for_each, suspend_100)
{
    return seastar::parallel_for_each(range, [this] (int v) {
        return suspend(v, value);
    }).then([this] {
        perf_tests::do_not_optimize(value);
        return range.size();
    });
}

PERF_TEST_C(parallel_for_each, cor_empty)
{
    co_await seastar::parallel_for_each(empty_range, [] (int) -> future<> {
        abort();
    });
}

PERF_TEST_CN(parallel_for_each, cor_immediate_1)
{
    constexpr size_t n = 1;
    auto&& begin = range.begin();
    auto&& end = begin + n;
    co_await seastar::parallel_for_each(std::move(begin), std::move(end), [this] (int v) {
        return immediate(v, value);
    });
    perf_tests::do_not_optimize(value);
    co_return n;
}

PERF_TEST_CN(parallel_for_each, cor_immediate_2)
{
    constexpr size_t n = 2;
    auto&& begin = range.begin();
    auto&& end = begin + n;
    co_await seastar::parallel_for_each(std::move(begin), std::move(end), [this] (int v) {
        return immediate(v, value);
    });
    perf_tests::do_not_optimize(value);
    co_return n;
}

PERF_TEST_CN(parallel_for_each, cor_immediate_10)
{
    constexpr size_t n = 10;
    auto&& begin = range.begin();
    auto&& end = begin + n;
    co_await seastar::parallel_for_each(std::move(begin), std::move(end), [this] (int v) {
        return immediate(v, value);
    });
    perf_tests::do_not_optimize(value);
    co_return n;
}

PERF_TEST_CN(parallel_for_each, cor_immediate_100)
{
    co_await seastar::parallel_for_each(range, [this] (int v) {
        return immediate(v, value);
    });
    perf_tests::do_not_optimize(value);
    co_return range.size();
}

PERF_TEST_CN(parallel_for_each, cor_suspend_1)
{
    constexpr size_t n = 1;
    auto&& begin = range.begin();
    auto&& end = begin + n;
    co_await seastar::parallel_for_each(std::move(begin), std::move(end), [this] (int v) {
        return suspend(v, value);
    });
    perf_tests::do_not_optimize(value);
    co_return n;
}

PERF_TEST_CN(parallel_for_each, cor_suspend_2)
{
    constexpr size_t n = 2;
    auto&& begin = range.begin();
    auto&& end = begin + n;
    co_await seastar::parallel_for_each(std::move(begin), std::move(end), [this] (int v) {
        return suspend(v, value);
    });
    perf_tests::do_not_optimize(value);
    co_return n;
}

PERF_TEST_CN(parallel_for_each, cor_suspend_10)
{
    constexpr size_t n = 10;
    auto&& begin = range.begin();
    auto&& end = begin + n;
    co_await seastar::parallel_for_each(std::move(begin), std::move(end), [this] (int v) {
        return suspend(v, value);
    });
    perf_tests::do_not_optimize(value);
    co_return n;
}

PERF_TEST_CN(parallel_for_each, cor_suspend_100)
{
    co_await seastar::parallel_for_each(range, [this] (int v) {
        return suspend(v, value);
    });
    perf_tests::do_not_optimize(value);
    co_return range.size();
}

PERF_TEST_C(parallel_for_each, cor_pfe_empty)
{
    co_await seastar::coroutine::parallel_for_each(empty_range, [] (int) -> future<> {
        abort();
    });
}

PERF_TEST_CN(parallel_for_each, cor_pfe_immediate_1)
{
    constexpr size_t n = 1;
    auto&& begin = range.begin();
    auto&& end = begin + n;
    co_await seastar::coroutine::parallel_for_each(std::move(begin), std::move(end), [this] (int v) {
        return immediate(v, value);
    });
    perf_tests::do_not_optimize(value);
    co_return n;
}

PERF_TEST_CN(parallel_for_each, cor_pfe_immediate_2)
{
    constexpr size_t n = 2;
    auto&& begin = range.begin();
    auto&& end = begin + n;
    co_await seastar::coroutine::parallel_for_each(std::move(begin), std::move(end), [this] (int v) {
        return immediate(v, value);
    });
    perf_tests::do_not_optimize(value);
    co_return n;
}

PERF_TEST_CN(parallel_for_each, cor_pfe_immediate_10)
{
    constexpr size_t n = 10;
    auto&& begin = range.begin();
    auto&& end = begin + n;
    co_await seastar::coroutine::parallel_for_each(std::move(begin), std::move(end), [this] (int v) {
        return immediate(v, value);
    });
    perf_tests::do_not_optimize(value);
    co_return n;
}

PERF_TEST_CN(parallel_for_each, cor_pfe_immediate_100)
{
    co_await seastar::coroutine::parallel_for_each(range, [this] (int v) {
        return immediate(v, value);
    });
    perf_tests::do_not_optimize(value);
    co_return range.size();
}

PERF_TEST_CN(parallel_for_each, cor_pfe_suspend_1)
{
    constexpr size_t n = 1;
    auto&& begin = range.begin();
    auto&& end = begin + n;
    co_await seastar::coroutine::parallel_for_each(std::move(begin), std::move(end), [this] (int v) {
        return suspend(v, value);
    });
    perf_tests::do_not_optimize(value);
    co_return n;
}

PERF_TEST_CN(parallel_for_each, cor_pfe_suspend_2)
{
    constexpr size_t n = 2;
    auto&& begin = range.begin();
    auto&& end = begin + n;
    co_await seastar::coroutine::parallel_for_each(std::move(begin), std::move(end), [this] (int v) {
        return suspend(v, value);
    });
    perf_tests::do_not_optimize(value);
    co_return n;
}

PERF_TEST_CN(parallel_for_each, cor_pfe_suspend_10)
{
    constexpr size_t n = 10;
    auto&& begin = range.begin();
    auto&& end = begin + n;
    co_await seastar::coroutine::parallel_for_each(std::move(begin), std::move(end), [this] (int v) {
        return suspend(v, value);
    });
    perf_tests::do_not_optimize(value);
    co_return n;
}

PERF_TEST_CN(parallel_for_each, cor_pfe_suspend_100)
{
    co_await seastar::coroutine::parallel_for_each(range, [this] (int v) {
        return suspend(v, value);
    });
    perf_tests::do_not_optimize(value);
    co_return range.size();
}
