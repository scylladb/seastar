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

#pragma once

#include <boost/preprocessor/control/iif.hpp>
#include <boost/preprocessor/comparison/equal.hpp>
#include <boost/preprocessor/variadic/size.hpp>

#include <seastar/core/future.hh>

#include <seastar/testing/seastar_test.hh>
#include <utility>

#define SEASTAR_TEST_CASE_WITH_DECO(name, decorators)               \
    struct name : public seastar::testing::seastar_test {           \
        using seastar::testing::seastar_test::seastar_test;         \
        seastar::future<> run_test_case() override;                 \
    };                                                              \
    static const name name ## _instance(                            \
        #name,                                                      \
        __FILE__,                                                   \
        __LINE__,                                                   \
        decorators); /* NOLINT(cert-err58-cpp) */                   \
    seastar::future<> name::run_test_case()

#define SEASTAR_TEST_CASE_WITHOUT_DECO(name)                        \
    SEASTAR_TEST_CASE_WITH_DECO(                                    \
        name,                                                       \
        boost::unit_test::decorator::collector_t::instance())

#define SEASTAR_TEST_CASE(...)                                      \
    SEASTAR_TEST_INVOKE(                                            \
        BOOST_PP_IIF(                                               \
            BOOST_PP_EQUAL(BOOST_PP_VARIADIC_SIZE(__VA_ARGS__), 1), \
            SEASTAR_TEST_CASE_WITHOUT_DECO,                         \
            SEASTAR_TEST_CASE_WITH_DECO),                           \
        __VA_ARGS__)

namespace seastar::testing::internal {
    template<typename T, typename = void>
    struct has_setup : std::false_type {};
    template<typename T>
    struct has_setup<T, std::void_t<decltype(std::declval<T>().setup())>> : std::true_type {};
    template<typename T>
    seastar::future<> setup_conditional(T& t) {
        if constexpr(has_setup<T>::value) {
            return t.setup();
        }
        return seastar::make_ready_future();
    }

    template<typename T, typename = void>
    struct has_teardown : std::false_type {};
    template<typename T>
    struct has_teardown<T, std::void_t<decltype(std::declval<T>().teardown())>> : std::true_type {};
    template<typename T>
    seastar::future<> teardown_conditional(T& t) {
        if constexpr(has_setup<T>::value) {
            return t.teardown();
        }
        return seastar::make_ready_future();
    }
}

#define SEASTAR_FIXTURE_TEST_CASE_WITH_DECO(name, F, decorators)        \
    struct name : public seastar::testing::seastar_test,                \
                  private F {                                           \
        using seastar::testing::seastar_test::seastar_test;             \
        seastar::future<> run_test_case() override;                     \
    private:                                                            \
        seastar::future<> do_run_test_case() const;                     \
    };                                                                  \
    static const name name ## _instance(                                \
        #name,                                                          \
        __FILE__,                                                       \
        __LINE__,                                                       \
        decorators); /* NOLINT(cert-err58-cpp) */                       \
    seastar::future<> name::run_test_case() {                           \
        BOOST_TEST_CHECKPOINT('"' << #name << "\" fixture ctor");       \
        BOOST_TEST_CHECKPOINT('"' << #name << "\" fixture setup");      \
        /* seastar_test does not have  adefault ctor,  */               \
        /* but setup_conditional needs to construct the fixture  */     \
        /* to check if setup() is available, so cast it to F */         \
        F& fixture = *this;                                             \
        return testing::internal::setup_conditional(fixture).then([this] { \
            BOOST_TEST_CHECKPOINT('"' << #name << "\" test entry");     \
            return do_run_test_case();                                  \
        }).finally([this] {                                             \
            BOOST_TEST_CHECKPOINT('"' << #name << "\" fixture teardown"); \
            F& fixture = *this;                                         \
            return testing::internal::teardown_conditional(fixture);    \
        });                                                             \
        BOOST_TEST_CHECKPOINT('"' << #name << "\" fixture dtor");       \
        return seastar::make_ready_future();                            \
    }                                                                   \
    seastar::future<> name::do_run_test_case() const


#define SEASTAR_FIXTURE_TEST_CASE_WITHOUT_DECO(name, F)             \
    SEASTAR_FIXTURE_TEST_CASE_WITH_DECO(                            \
        name, F,                                                    \
        boost::unit_test::decorator::collector_t::instance())

#define SEASTAR_FIXTURE_TEST_CASE(...)                              \
    SEASTAR_TEST_INVOKE(                                            \
        BOOST_PP_IIF(                                               \
            BOOST_PP_EQUAL(BOOST_PP_VARIADIC_SIZE(__VA_ARGS__), 2), \
            SEASTAR_FIXTURE_TEST_CASE_WITHOUT_DECO,                 \
            SEASTAR_FIXTURE_TEST_CASE_WITH_DECO),                   \
        __VA_ARGS__)
