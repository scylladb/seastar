/*
 * Copyright (C) 2025-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: LicenseRef-ScyllaDB-Source-Available-1.0
 */

#include <gnutls/gnutls.h>
#include <seastar/http/exception.hh>
#include <seastar/http/retry_strategy.hh>

using namespace std::chrono_literals;
namespace seastar::http::experimental {
logger rs_logger("default_retry_strategy");
static bool is_retryable_exception(std::exception_ptr ex) {
    while (ex) {
        try {
            std::rethrow_exception(ex);
        } catch (const std::system_error& sys_err) {
            auto code = sys_err.code().value();
            if (code == EPIPE || code == ECONNABORTED || code == GNUTLS_E_PREMATURE_TERMINATION) {
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
future<bool> default_connection_reset_strategy::should_retry(const std::exception_ptr& error, unsigned attempted_retries) const {
    if (attempted_retries >= get_max_retries()) {
        rs_logger.warn("Retries exhausted. Retry# {}", attempted_retries);
        co_return false;
    }

    co_return is_retryable_exception(error);
}
std::chrono::milliseconds default_connection_reset_strategy::delay_before_retry(const std::exception_ptr&, unsigned) const {
    return 0ms;
}
} // namespace seastar::http::experimental
