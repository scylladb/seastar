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

default_retry_strategy::default_retry_strategy(unsigned max_retries)
    : _max_retries(max_retries) {
}

default_retry_strategy::default_retry_strategy()
    : default_retry_strategy(1) {
}

future<bool> default_retry_strategy::should_retry(std::exception_ptr error, unsigned attempted_retries) const {
    if (attempted_retries >= _max_retries) {
        rs_logger.debug("Retries exhausted. Retry# {}", attempted_retries);
        co_return false;
    }

    co_return is_retryable_exception(error);
}

future<bool> no_retry_strategy::should_retry(std::exception_ptr error, unsigned) const {
    co_return false;
}

}
}
