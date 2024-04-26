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
 * Copyright (C) 2022 Kefu Chai ( tchaikov@gmail.com )
 */
#include <exception>
#include <system_error>
#include <seastar/core/reactor.hh>
#include <seastar/core/seastar.hh>
#include <seastar/testing/test_case.hh>
#include <seastar/util/log.hh>
#include <seastar/util/process.hh>

using namespace seastar;
using namespace seastar::experimental;

static seastar::logger testlog("testlog");

SEASTAR_TEST_CASE(test_spawn_success) {
    auto process = co_await spawn_process("/bin/true");
    auto wstatus = co_await process.wait();
    auto* exit_status = std::get_if<process::wait_exited>(&wstatus);
    BOOST_REQUIRE(exit_status != nullptr);
    BOOST_CHECK_EQUAL(exit_status->exit_code, EXIT_SUCCESS);
}

SEASTAR_TEST_CASE(test_spawn_failure) {
    auto process = co_await spawn_process("/bin/false");
    auto wstatus = co_await process.wait();
    auto* exit_status = std::get_if<process::wait_exited>(&wstatus);
    BOOST_REQUIRE(exit_status != nullptr);
    BOOST_CHECK_EQUAL(exit_status->exit_code, EXIT_FAILURE);
}

SEASTAR_TEST_CASE(test_spawn_program_does_not_exist) {
    BOOST_CHECK_EXCEPTION(co_await spawn_process("non/existent/path"),
                          std::system_error,
                          [](const auto& e) {
                              return e.code().value() == ENOENT;
                          });
}

SEASTAR_TEST_CASE(test_spawn_echo) {
    const char* echo_cmd = "/bin/echo";
    auto process = co_await spawn_process(echo_cmd, {.argv = {echo_cmd, "-n", "hello", "world"}});
    auto cout = process.cout();
    using consumption_result_type = typename input_stream<char>::consumption_result_type;
    using stop_consuming_type = typename consumption_result_type::stop_consuming_type;
    using tmp_buf = stop_consuming_type::tmp_buf;
    struct consumer {
        consumer(std::string_view expected, bool& matched)
            : _expected(expected), _matched(matched) {}
        future<consumption_result_type> operator()(tmp_buf buf) {
            if (!std::equal(buf.begin(), buf.end(), _expected.begin())) {
                _matched = false;
                return make_ready_future<consumption_result_type>(stop_consuming_type({}));
            }
            _expected.remove_prefix(buf.size());
            if (_expected.empty()) {
                _matched = true;
                return make_ready_future<consumption_result_type>(stop_consuming_type({}));
            }
            return make_ready_future<consumption_result_type>(continue_consuming{});
        }
        std::string_view _expected;
        bool& _matched;
    };
    bool matched = false;
    BOOST_CHECK_NO_THROW(co_await cout.consume(consumer("hello world", matched)));
    BOOST_CHECK(matched);
    [[maybe_unused]] auto wstatus = co_await process.wait();
}

SEASTAR_TEST_CASE(test_spawn_input) {
    auto process = co_await spawn_process("/bin/cat");
    static const sstring text = "hello world\n";
    try {
        auto cin = process.cin();
        co_await cin.write(text);
        co_await cin.close();
    } catch (const std::system_error& e) {
        BOOST_TEST_ERROR(fmt::format("failed to write to stdin: {}", e));
    }
    temporary_buffer<char> echo;
    auto cout = process.cout();
    BOOST_CHECK_NO_THROW(echo = co_await cout.read_exactly(text.size()));
    co_await cout.close();
    BOOST_CHECK_EQUAL(sstring(echo.get(), echo.size()), text);
    process::wait_status wstatus = co_await process.wait();
    auto* exit_status = std::get_if<process::wait_exited>(&wstatus);
    BOOST_REQUIRE(exit_status != nullptr);
    BOOST_CHECK_EQUAL(exit_status->exit_code, EXIT_SUCCESS);
}

SEASTAR_TEST_CASE(test_spawn_kill) {
    const char* sleep_cmd = "/bin/sleep";
    // sleep for 10s, but terminate it right away.
    auto process = co_await spawn_process(sleep_cmd, {.argv = {sleep_cmd, "10"}});
    auto start = std::chrono::high_resolution_clock::now();
    process.terminate();
    experimental::process::wait_status wait_status = co_await process.wait();
    auto* wait_signaled = std::get_if<experimental::process::wait_signaled>(&wait_status);
    BOOST_REQUIRE(wait_signaled != nullptr);
    BOOST_CHECK_EQUAL(wait_signaled->terminating_signal, SIGTERM);
    auto end = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    // sleep should be terminated in 10ms.
    // pidfd_open(2) may fail and thus p.wait() falls back to
    // waitpid(2) with backoff (at least 20ms).
    // the minimal backoff is added to 10ms, so the test can pass on
    // older kernels as well.
    BOOST_CHECK_LE(ms, 10 + 20);
}
