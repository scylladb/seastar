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
#include <seastar/core/reactor.hh>
#include <seastar/core/seastar.hh>
#include <seastar/testing/test_case.hh>
#include <seastar/util/log.hh>
#include <seastar/util/process.hh>

using namespace seastar;
using namespace seastar::experimental;

static seastar::logger testlog("testlog");

SEASTAR_TEST_CASE(test_spawn_success) {
    return spawn_process("/bin/true").then([] (auto process) {
        return process.wait();
    }).then([] (auto wstatus) {
        auto* exit_status = std::get_if<process::wait_exited>(&wstatus);
        BOOST_REQUIRE(exit_status != nullptr);
        BOOST_CHECK_EQUAL(exit_status->exit_code, EXIT_SUCCESS);
    });
}

SEASTAR_TEST_CASE(test_spawn_failure) {
    return spawn_process("/bin/false").then([] (auto process) {
        return process.wait();
    }).then([] (auto wstatus) {
        auto* exit_status = std::get_if<process::wait_exited>(&wstatus);
        BOOST_REQUIRE(exit_status != nullptr);
        BOOST_CHECK_EQUAL(exit_status->exit_code, EXIT_FAILURE);
    });
}

SEASTAR_TEST_CASE(test_spawn_program_does_not_exist) {
    return spawn_process("non/existent/path").then_wrapped([] (future<process> fut) {
        BOOST_REQUIRE(fut.failed());
        BOOST_CHECK_EXCEPTION(std::rethrow_exception(fut.get_exception()),
                              std::system_error,
                              [](const auto& e) {
                                  return e.code().value() == ENOENT;
                              });
    });
}

SEASTAR_TEST_CASE(test_spawn_echo) {
    const char* echo_cmd = "/bin/echo";
    return spawn_process(echo_cmd, {.argv = {echo_cmd, "-n", "hello", "world"}}).then([] (auto process) {
        auto stdout = process.stdout();
        return do_with(std::move(process), std::move(stdout), bool(true), [](auto& p, auto& stdout, auto& matched) {
            using consumption_result_type = typename input_stream<char>::consumption_result_type;
            using stop_consuming_type = typename consumption_result_type::stop_consuming_type;
            using tmp_buf = stop_consuming_type::tmp_buf;
            struct consumer {
                consumer(std::string_view expected, bool& matched)
                    : _expected(expected), _matched(matched) {}
                future<consumption_result_type> operator()(tmp_buf buf) {
                    _matched = std::equal(buf.begin(), buf.end(), _expected.begin());
                    if (!_matched) {
                        return make_ready_future<consumption_result_type>(stop_consuming_type({}));
                    }
                    _expected.remove_prefix(buf.size());
                    return make_ready_future<consumption_result_type>(continue_consuming{});
                }
                std::string_view _expected;
                bool& _matched;
            };
            return stdout.consume(consumer("hello world", matched)).then([&matched] {
                BOOST_CHECK(matched);
            }).finally([&p] {
                return p.wait().discard_result();
            });
        });
    });
}

SEASTAR_TEST_CASE(test_spawn_input) {
    static const sstring text = "hello world\n";
    return spawn_process("/bin/cat").then([] (auto process) {
        auto stdin = process.stdin();
        auto stdout = process.stdout();
        return do_with(std::move(process), std::move(stdin), std::move(stdout), [](auto& p, auto& stdin, auto& stdout) {
            return stdin.write(text).then([&stdin] {
                return stdin.flush();
            }).handle_exception_type([] (std::system_error& e) {
                testlog.error("failed to write to stdin: {}", e);
                return make_exception_future<>(std::move(e));
            }).then([&stdout] {
                return stdout.read_exactly(text.size());
            }).handle_exception_type([] (std::system_error& e) {
                testlog.error("failed to read from stdout: {}", e);
                return make_exception_future<temporary_buffer<char>>(std::move(e));
            }).then([] (temporary_buffer<char> echo) {
                BOOST_CHECK_EQUAL(sstring(echo.get(), echo.size()), text);
            }).finally([&p] {
                return p.wait().discard_result();
            });
        });
    });
}

SEASTAR_TEST_CASE(test_spawn_kill) {
    const char* sleep_cmd = "/bin/sleep";
    // sleep for 10s, but terminate it right away.
    return spawn_process(sleep_cmd, {.argv = {sleep_cmd, "10"}}).then([] (auto process) {
        auto start = std::chrono::high_resolution_clock::now();
        return do_with(std::move(process), [](auto& p) {
            p.terminate();
            return p.wait();
        }).then([start](experimental::process::wait_status wait_status) {
            auto* wait_signaled = std::get_if<experimental::process::wait_signaled>(&wait_status);
            BOOST_REQUIRE(wait_signaled != nullptr);
            BOOST_CHECK_EQUAL(wait_signaled->terminating_signal, SIGTERM);
            // sleep should be terminated in 10ms
            auto end = std::chrono::high_resolution_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
            BOOST_CHECK_LE(ms, 10);
        });
    });
}
