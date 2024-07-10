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
 * Copyright 2020 ScyllaDB
 */

#ifdef SEASTAR_MODULE
module;
#endif

#include <atomic>
#include <exception>
#include <stdexcept>
#include <string_view>
#include <cstdlib>

#ifdef SEASTAR_MODULE
module seastar;
#else
#include <seastar/core/on_internal_error.hh>
#include <seastar/util/backtrace.hh>
#include <seastar/util/log.hh>
#endif

static std::atomic<bool> abort_on_internal_error{false};

using namespace seastar;

bool seastar::set_abort_on_internal_error(bool do_abort) noexcept {
    return abort_on_internal_error.exchange(do_abort);
}

template <typename Message>
static void log_error_and_backtrace(logger& logger, const Message& msg) noexcept {
    logger.error("{}, at: {}", msg, current_backtrace());
}

void seastar::on_internal_error(logger& logger, std::string_view msg) {
    log_error_and_backtrace(logger, msg);
    if (abort_on_internal_error.load()) {
        abort();
    } else {
        throw_with_backtrace<std::runtime_error>(std::string(msg));
    }
}

void seastar::on_internal_error(logger& logger, std::exception_ptr ex) {
    log_error_and_backtrace(logger, ex);
    if (abort_on_internal_error.load()) {
        abort();
    } else {
        std::rethrow_exception(std::move(ex));
    }
}

void seastar::on_internal_error_noexcept(logger& logger, std::string_view msg) noexcept {
    log_error_and_backtrace(logger, msg);
    if (abort_on_internal_error.load()) {
        abort();
    }
}

void seastar::on_fatal_internal_error(logger& logger, std::string_view msg) noexcept {
    log_error_and_backtrace(logger, msg);
    abort();
}
