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

#ifdef SEASTAR_MODULE
module seastar;
#else
#include <seastar/http/exception.hh>
#include <seastar/http/retry_strategy.hh>
#endif

using namespace std::chrono_literals;

namespace seastar::http::experimental {
logger rs_logger("default_retry_strategy");

static bool is_retryable_exception(std::exception_ptr ex) {
    return false;
}

default_connection_reset_strategy::default_connection_reset_strategy(unsigned max_retries, unsigned scale_factor, retry_requests client_should_retry)
    : _client_should_retry(client_should_retry), _max_retries(max_retries), _scale_factor(scale_factor) {
}

default_connection_reset_strategy::default_connection_reset_strategy(retry_requests client_should_retry)
    : default_connection_reset_strategy(2, 25, client_should_retry) /* Default overall retry time to fail is 50ms (0ms)+((1<<1)*25)ms) */ {
}

future<bool> default_connection_reset_strategy::should_retry(const std::exception_ptr& error, unsigned attempted_retries) const {
    if (_client_should_retry == retry_requests::no || attempted_retries >= get_max_retries()) {
        rs_logger.warn("Retries exhausted. Retry# {}", attempted_retries);
        co_return false;
    }

    co_return is_retryable_exception(error);
}

std::chrono::milliseconds default_connection_reset_strategy::delay_before_retry(const std::exception_ptr&, unsigned attempted_retries) const {
    if (_client_should_retry == retry_requests::no || attempted_retries <= 1) {
        // On first attempt, we do not delay
        return 0ms;
    }

    return std::chrono::milliseconds((1UL << (attempted_retries - 1)) * _scale_factor);
}

unsigned default_connection_reset_strategy::get_max_retries() const {
    return retry_requests::yes == _client_should_retry ? _max_retries : 0;
}
} // namespace seastar::http::experimental
