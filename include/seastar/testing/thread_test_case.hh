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

#include <seastar/core/future.hh>
#include <seastar/core/thread.hh>

#include <boost/preprocessor/control/iif.hpp>
#include <boost/preprocessor/comparison/equal.hpp>
#include <boost/preprocessor/variadic/size.hpp>

#include <seastar/testing/seastar_test.hh>

#define SEASTAR_THREAD_TEST_CASE_WITH_DECO(name, decorators) \
    struct name : public seastar::testing::seastar_test {    \
        using seastar::testing::seastar_test::seastar_test;  \
        seastar::future<> run_test_case() override {         \
            return seastar::async([this] {                   \
                do_run_test_case();                          \
            });                                              \
        }                                                    \
        void do_run_test_case() const;                       \
    };                                                       \
    static const name name ## _instance(                     \
        #name,                                               \
        __FILE__,                                            \
        __LINE__,                                            \
        decorators); /* NOLINT(cert-err58-cpp) */            \
    void name::do_run_test_case() const

#define SEASTAR_THREAD_TEST_CASE_WITHOUT_DECO(name)          \
    SEASTAR_THREAD_TEST_CASE_WITH_DECO(                      \
        name,                                                \
        boost::unit_test::decorator::collector_t::instance())

#define SEASTAR_THREAD_TEST_CASE(...)                               \
    SEASTAR_TEST_INVOKE(                                            \
        BOOST_PP_IIF(                                               \
            BOOST_PP_EQUAL(BOOST_PP_VARIADIC_SIZE(__VA_ARGS__), 1), \
            SEASTAR_THREAD_TEST_CASE_WITHOUT_DECO,                  \
            SEASTAR_THREAD_TEST_CASE_WITH_DECO),                    \
        __VA_ARGS__)

#define SEASTAR_THREAD_FIXTURE_TEST_CASE_WITH_DECO(name, F, decorators) \
    struct name : public seastar::testing::seastar_test, public F {     \
        using seastar::testing::seastar_test::seastar_test;             \
        seastar::future<> run_test_case() override;                     \
    private:                                                            \
        void do_run_test_case() const;                                  \
    };                                                                  \
    static const name name ## _instance(                                \
        #name,                                                          \
        __FILE__,                                                       \
        __LINE__,                                                       \
        decorators); /* NOLINT(cert-err58-cpp) */                       \
    seastar::future<> name::run_test_case() {                           \
        return seastar::async([this] {                                  \
            BOOST_TEST_CHECKPOINT('"' << #name << "\" fixture ctor");   \
            BOOST_TEST_CHECKPOINT('"' << #name << "\" fixture setup");  \
            /* seastar_test does not have a default ctor,  */           \
            /* but setup_conditional needs to construct the fixture  */ \
            /* to check if setup() is available, so cast it to F */     \
            F& fixture = *this;                                         \
            boost::unit_test::setup_conditional(fixture);               \
            BOOST_TEST_CHECKPOINT('"' << #name << "\" test entry");     \
            do_run_test_case();                                         \
            BOOST_TEST_CHECKPOINT('"' << #name << "\" fixture teardown"); \
            boost::unit_test::teardown_conditional(fixture);            \
            BOOST_TEST_CHECKPOINT('"' << #name << "\" fixture dtor");   \
        });                                                             \
    }                                                                   \
    void name::do_run_test_case() const                                 \


#define SEASTAR_THREAD_FIXTURE_TEST_CASE_WITHOUT_DECO(name, F)      \
    SEASTAR_THREAD_FIXTURE_TEST_CASE_WITH_DECO(                     \
        name, F,                                                    \
        boost::unit_test::decorator::collector_t::instance())

#define SEASTAR_THREAD_FIXTURE_TEST_CASE(...)                       \
    SEASTAR_TEST_INVOKE(                                            \
        BOOST_PP_IIF(                                               \
            BOOST_PP_EQUAL(BOOST_PP_VARIADIC_SIZE(__VA_ARGS__), 2), \
            SEASTAR_THREAD_FIXTURE_TEST_CASE_WITHOUT_DECO,          \
            SEASTAR_THREAD_FIXTURE_TEST_CASE_WITH_DECO),            \
        __VA_ARGS__)
