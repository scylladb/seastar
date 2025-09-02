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

static bool is_retryable_exception(std::exception_ptr ex) {
    while (ex) {
        try {
            std::rethrow_exception(ex);
        } catch (const std::system_error& sys_err) {
            auto code = sys_err.code().value();
            if (code == EPIPE || code == ECONNABORTED || code == ECONNRESET || code == GNUTLS_E_PREMATURE_TERMINATION) {
                return true;
            }
            try {
                std::rethrow_if_nested(sys_err);
            } catch (...) {
                ex = std::current_exception();
                continue;
            }
            return false;
        } catch (const httpd::response_parsing_exception&) {
            return true;
        } catch (const std::exception& e) {
            try {
                std::rethrow_if_nested(e);
            } catch (...) {
                ex = std::current_exception();
                continue;
            }
            return false;
        } catch (...) {
            return false;
        }
    }
    return false;
}

default_retry_strategy::default_retry_strategy(unsigned max_retries)
    : _max_retries(max_retries) {
}

default_retry_strategy::default_retry_strategy()
    : default_retry_strategy(1) {
}

future<bool> default_retry_strategy::should_retry(std::exception_ptr error, unsigned attempted_retries) const {
    if (attempted_retries >= get_max_retries()) {
        rs_logger.warn("Retries exhausted. Retry# {}", attempted_retries);
        co_return false;
    }

    co_return is_retryable_exception(error);
}

unsigned default_retry_strategy::get_max_retries() const noexcept {
    return _max_retries;
}

future<bool> no_retry_strategy::should_retry(std::exception_ptr error, unsigned attempted_retries) const {
    return make_ready_future<bool>(false);
}

unsigned no_retry_strategy::get_max_retries() const noexcept {
    return 0;
}
}
}
