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

#define BOOST_TEST_MODULE core

#include <boost/test/unit_test.hpp>
#include <seastar/core/on_internal_error.hh>
#include <seastar/util/log.hh>

#include <array>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

constexpr std::string_view reason = "internal error regression test";
constexpr std::string_view abort_prefix = "Aborting on internal error: ";

enum class outcome { returns, throws, aborts };

struct error_case {
    const char* name;
    void (*report)(seastar::logger&);
    outcome without_abort_setting;
};

[[noreturn]] void run_child(const error_case& test, bool abort_enabled, int log_fd) {
    const rlimit no_core{0, 0};
    if (setrlimit(RLIMIT_CORE, &no_core) != 0
            || std::signal(SIGABRT, SIG_DFL) == SIG_ERR
            || dup2(log_fd, STDERR_FILENO) == -1) {
        _exit(EXIT_FAILURE);
    }

    seastar::logger::set_ostream(std::cerr);
    seastar::logger::set_ostream_enabled(true);
    seastar::logger::set_syslog_enabled(false);
    seastar::logger::set_with_color(false);
    seastar::set_abort_on_internal_error(abort_enabled);
    seastar::logger log("on_internal_error_test");

    bool threw = false;
    try {
        test.report(log);
    } catch (const std::runtime_error& ex) {
        threw = true;
        if (!std::string_view(ex.what()).starts_with(reason)) {
            _exit(EXIT_FAILURE);
        }
    } catch (...) {
        _exit(EXIT_FAILURE);
    }

    _exit(threw == (test.without_abort_setting == outcome::throws)
            ? EXIT_SUCCESS : EXIT_FAILURE);
}

void check_error(const error_case& test, bool abort_enabled) {
    std::unique_ptr<std::FILE, int (*)(std::FILE*)> output(std::tmpfile(), &std::fclose);
    BOOST_REQUIRE(output != nullptr);

    std::cout.flush();
    std::cerr.flush();
    const auto child = fork();
    if (child == 0) {
        run_child(test, abort_enabled, fileno(output.get()));
    }
    BOOST_REQUIRE(child > 0);

    int status = 0;
    auto waited = waitpid(child, &status, 0);
    while (waited == -1 && errno == EINTR) {
        waited = waitpid(child, &status, 0);
    }
    BOOST_REQUIRE_EQUAL(waited, child);

    const bool should_abort = abort_enabled || test.without_abort_setting == outcome::aborts;
    if (should_abort) {
        BOOST_REQUIRE(WIFSIGNALED(status));
        BOOST_CHECK_EQUAL(WTERMSIG(status), SIGABRT);
    } else {
        BOOST_REQUIRE(WIFEXITED(status));
        BOOST_CHECK_EQUAL(WEXITSTATUS(status), EXIT_SUCCESS);
    }

    BOOST_REQUIRE_EQUAL(std::fseek(output.get(), 0, SEEK_SET), 0);
    std::string message;
    std::array<char, 4'096> buffer{};
    while (const auto count = std::fread(buffer.data(), 1, buffer.size(), output.get())) {
        message.append(buffer.data(), count);
    }
    BOOST_REQUIRE_EQUAL(std::ferror(output.get()), 0);
    BOOST_CHECK(message.find(reason) != std::string::npos);
    BOOST_CHECK_EQUAL(message.find(abort_prefix) != std::string::npos, should_abort);
}

}

BOOST_AUTO_TEST_CASE(internal_error_logs_match_abort_behavior) {
    const error_case cases[] = {
        {"message", [](seastar::logger& log) {
            seastar::on_internal_error(log, reason);
        }, outcome::throws},
        {"exception", [](seastar::logger& log) {
            seastar::on_internal_error(log, std::make_exception_ptr(std::runtime_error(std::string(reason))));
        }, outcome::throws},
        {"noexcept", [](seastar::logger& log) {
            seastar::on_internal_error_noexcept(log, reason);
        }, outcome::returns},
        {"fatal", [](seastar::logger& log) {
            seastar::on_fatal_internal_error(log, reason);
        }, outcome::aborts},
    };

    for (const auto& test : cases) {
        for (bool abort_enabled : {false, true}) {
            BOOST_TEST_CONTEXT(test.name << ", abort enabled: " << abort_enabled) {
                check_error(test, abort_enabled);
            }
        }
    }
}
