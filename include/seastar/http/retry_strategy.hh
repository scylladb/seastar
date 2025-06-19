/*
 * Copyright (C) 2025-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: LicenseRef-ScyllaDB-Source-Available-1.0
 */

#pragma once
#include <seastar/core/future.hh>
#include <seastar/util/bool_class.hh>

namespace seastar {
SEASTAR_MODULE_EXPORT_BEGIN
namespace http::experimental {

using retry_requests = bool_class<struct retry_requests_tag>;

template <typename T>
class retry_strategy {
public:
    virtual ~retry_strategy() = default;
    // Returns true if the error can be retried given the error and the number of times already tried.
    virtual future<bool> should_retry(const T& error, unsigned attempted_retries) const = 0;

    // Calculates the time in milliseconds the client should wait before attempting another request based on the error and attemptedRetries count.
    [[nodiscard]] virtual std::chrono::milliseconds delay_before_retry(const T& error, unsigned attempted_retries) const = 0;

    [[nodiscard]] virtual unsigned get_max_retries() const = 0;
};

class default_connection_reset_strategy : public retry_strategy<std::exception_ptr> {
private:
    const retry_requests _client_should_retry;

public:
    explicit default_connection_reset_strategy(const retry_requests client_should_retry) : _client_should_retry(client_should_retry) {}

    future<bool> should_retry(const std::exception_ptr& error, unsigned attempted_retries) const override;

    [[nodiscard]] std::chrono::milliseconds delay_before_retry(const std::exception_ptr& error, unsigned attempted_retries) const override;

    [[nodiscard]] unsigned get_max_retries() const override { return retry_requests::yes == _client_should_retry ? 1 : 0; }
};

} // namespace http::experimental
SEASTAR_MODULE_EXPORT_END
} // namespace seastar
