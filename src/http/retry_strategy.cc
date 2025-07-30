/*
 * Copyright (C) 2025-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: LicenseRef-ScyllaDB-Source-Available-1.0
 */

#ifdef SEASTAR_MODULE
module;
#endif

#include <chrono>
#include <coroutine>
#include <gnutls/gnutls.h>

#ifdef SEASTAR_MODULE
module seastar;
#else
#include <seastar/http/exception.hh>
#include <seastar/http/retry_strategy.hh>
#include <seastar/util/short_streams.hh>

#endif

using namespace std::chrono_literals;

namespace seastar {
extern logger http_log;

namespace http::experimental {
logger rs_logger("default_http_retry_strategy");

extern bool is_retryable_exception(std::exception_ptr ex);

default_retry_strategy::default_retry_strategy(unsigned max_retries, unsigned scale_factor)
    : _max_retries(max_retries), _scale_factor(scale_factor) {
}

default_retry_strategy::default_retry_strategy()
    : default_retry_strategy(2, 25) /* Default overall retry time to fail is 50ms (0ms)+((1<<1)*25)ms) */ {
}

future<bool> default_retry_strategy::should_retry(std::exception_ptr error, unsigned attempted_retries) const {
    if (attempted_retries >= get_max_retries()) {
        rs_logger.warn("Retries exhausted. Retry# {}", attempted_retries);
        co_return false;
    }

    co_return is_retryable_exception(error);
}

future<input_stream<char>> default_retry_strategy::analyze_reply(std::optional<reply::status_type> expected, const reply& rep, input_stream<char>&& in) const {
    co_return std::move(in);
}

std::chrono::milliseconds default_retry_strategy::delay_before_retry(std::exception_ptr, unsigned attempted_retries) const {
    if (attempted_retries <= 1) {
        // On first attempt, we do not delay
        return 0ms;
    }

    return std::chrono::milliseconds((1UL << (attempted_retries - 1)) * _scale_factor);
}

unsigned default_retry_strategy::get_max_retries() const noexcept {
    return _max_retries;
}

future<bool> no_retry_strategy::should_retry(std::exception_ptr error, unsigned attempted_retries) const {
    return make_ready_future<bool>(false);
}

std::chrono::milliseconds no_retry_strategy::delay_before_retry(std::exception_ptr error, unsigned attempted_retries) const {
    return 0ms;
}

unsigned no_retry_strategy::get_max_retries() const noexcept {
    return 0;
}
}
}
