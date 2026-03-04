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
 * Copyright (C) 2025 ScyllaDB Ltd.
 */

#pragma once

#include <functional>

#include <seastar/core/future.hh> // IWYU pragma: keep
#include <seastar/core/thread.hh>
#include <seastar/util/defer.hh>
#include <seastar/testing/seastar_test.hh>
#include <seastar/testing/test_runner.hh>

namespace seastar::testing {
namespace detail {

    template<typename T>
future<> conditional_invoke_setup(T& t) {
    if constexpr (boost::unit_test::impl_fixture::has_setup<T>::value) {
        return futurize_invoke(std::mem_fn(&T::setup), t);
    }
    return make_ready_future<>();
}

template<typename T>
future<> conditional_invoke_teardown(T& t) {
    if constexpr (boost::unit_test::impl_fixture::has_teardown<T>::value) {
        return futurize_invoke(std::mem_fn(&T::teardown), t);
    }
    return make_ready_future<>();
}
void warn_teardown_exception(const sstring&, std::exception_ptr);

template<typename Func, typename T>
requires std::is_invocable_v<Func, T>
future<> do_fixture_test(Func&& f, T& t, const sstring& name) {
    return conditional_invoke_setup(t).then([f = std::forward<Func>(f), &t]() mutable {
        return futurize_invoke(std::forward<Func>(f), t);
    }).finally([&]() {
        return conditional_invoke_teardown(t).handle_exception([&](auto ep) {
            warn_teardown_exception(name, ep);
        });
    });
}
} // detail

// holder for decorator based fixture using class
template<typename F>
class async_class_based_fixture : public boost::unit_test::test_unit_fixture {
public:
    template<typename... Args>
    requires std::is_constructible_v<F, const Args&...>
    async_class_based_fixture(Args&& ...args)
        : _create([args = std::make_tuple(std::forward<Args>(args)...)] {
            return std::apply([](auto&& ...args) {
                return std::make_unique<F>(args...);
            }, args);
        })
    {}
private:
    // Fixture interface
    void setup() override {
        // create and possibly init the fixture object in reactor, on the
        // test thread.
        global_test_runner().run_sync([this] {
            _inst = _create();
            return detail::conditional_invoke_setup(*_inst);
        });
    }
    void teardown() override {
        // possibly de-init and destroy the fixture object in reactor, on the
        // test thread.
        global_test_runner().run_sync([this] {
            auto inst = std::exchange(_inst, {});
            return detail::conditional_invoke_teardown(*inst).finally([inst = std::move(inst)] {});
        });
    }
    // Data members
    std::unique_ptr<F> _inst;
    std::function<std::unique_ptr<F>()> _create;
};

using fixture_function = std::function<future<>()>;

// holder for decorator based fixture using functions
class async_function_based_fixture : public boost::unit_test::test_unit_fixture {
public:
    async_function_based_fixture(fixture_function setup, fixture_function teardown)
        : _setup(std::move(setup))
        , _teardown(std::move(teardown))
    {}
private:
    // Fixture interface
    void setup() override {
        if (_setup) {
            // run in reactor thread
            global_test_runner().run_sync([this] {
                return _setup();
            });
        }
    }
    void teardown() override {
        if (_teardown) {
            // run in reactor thread
            global_test_runner().run_sync([this] {
                return _teardown();
            });
        }
    }
    fixture_function _setup, _teardown;
};

// our versions of "fixture" in boost. only differs in decorator used
template<typename F, typename... Args>
requires std::is_constructible_v<F, const Args&...>
inline boost::unit_test::decorator::fixture_t async_fixture(Args&& ...args) {
    return boost::unit_test::decorator::fixture_t(
        boost::unit_test::test_unit_fixture_ptr(new async_class_based_fixture<F>(std::forward<Args>(args)...))
    );
}
inline boost::unit_test::decorator::fixture_t async_fixture(fixture_function setup, fixture_function teardown) {
    return boost::unit_test::decorator::fixture_t(
        boost::unit_test::test_unit_fixture_ptr(new async_function_based_fixture(std::move(setup), std::move(teardown)))
    );
}
template<typename F1, typename F2>
requires (std::is_invocable_v<F1> && std::is_invocable_v<F2>)
inline boost::unit_test::decorator::fixture_t async_func_fixture(F1 setup, F2 teardown) {
    return boost::unit_test::decorator::fixture_t(
        boost::unit_test::test_unit_fixture_ptr(new async_function_based_fixture(std::move(setup), std::move(teardown)))
    );
}
}

// macro for using a base-class type fixture with async setup/teardown
// this just piggy-backs on normal seastar test case.
#define SEASTAR_FIXTURE_TEST_CASE(name, F, ...)                     \
    struct name ## _fxt : public F {                                \
        seastar::future<> run_test_case() const;                    \
    };                                                              \
    SEASTAR_TEST_CASE(name, ##__VA_ARGS__) {                        \
        namespace td =  seastar::testing::detail;                   \
        using type = name ## _fxt;                                  \
        auto t = std::make_unique<type>();                          \
        return td::do_fixture_test(                                 \
                std::mem_fn(&type::run_test_case), *t, #F           \
            ).finally([t = std::move(t)] {});                       \
    }                                                               \
    seastar::future<> name ## _fxt::run_test_case() const

#define SEASTAR_FIXTURE_THREAD_TEST_CASE(name, F, ...)              \
    struct name ## _fxt : public F {                                \
        void run_test_case() const;                                 \
    };                                                              \
    SEASTAR_TEST_CASE(name, ##__VA_ARGS__) {                        \
        return seastar::async([] {                                  \
            namespace td =  seastar::testing::detail;               \
            using type = name ## _fxt;                              \
            type t;                                                 \
            td::do_fixture_test(                                    \
                std::mem_fn(&type::run_test_case), t, #F            \
                ).get();                                            \
        });                                                         \
    }                                                               \
    void name ## _fxt::run_test_case() const
