/*
 * Copyright (C) 2025-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: LicenseRef-ScyllaDB-Source-Available-1.0
 */

#pragma once
#include <seastar/core/future.hh>
#include <seastar/http/reply.hh>
#include <seastar/util/modules.hh>

namespace seastar {
SEASTAR_MODULE_EXPORT_BEGIN

namespace http::experimental {

class retry_strategy {
public:
    virtual ~retry_strategy() = default;
    // Returns true if the error can be retried given the error and the number of times already tried.
    virtual future<bool> should_retry(std::exception_ptr error, unsigned attempted_retries) const = 0;
    virtual future<input_stream<char>> analyze_reply(std::optional<reply::status_type> expected, const http::reply& rep, input_stream<char>&& in) const = 0;

    // Calculates the time in milliseconds the client should wait before attempting another request based on the error and attempted_retries count.
    [[nodiscard]] virtual std::chrono::milliseconds delay_before_retry(std::exception_ptr error, unsigned attempted_retries) const = 0;

    [[nodiscard]] virtual unsigned get_max_retries() const noexcept = 0;
};

class default_retry_strategy : public retry_strategy {
private:
    unsigned _max_retries;
    unsigned _scale_factor;

public:
    default_retry_strategy();
    default_retry_strategy(unsigned max_retries, unsigned scale_factor);

    future<bool> should_retry(std::exception_ptr error, unsigned attempted_retries) const override;
    future<input_stream<char>> analyze_reply(std::optional<reply::status_type> expected, const http::reply& rep, input_stream<char>&& in) const override;

    [[nodiscard]] std::chrono::milliseconds delay_before_retry(std::exception_ptr error, unsigned attempted_retries) const override;

    [[nodiscard]] unsigned get_max_retries() const noexcept override;
};

class no_retry_strategy : public default_retry_strategy {

public:
    future<bool> should_retry(std::exception_ptr error, unsigned attempted_retries) const override;
    [[nodiscard]] std::chrono::milliseconds delay_before_retry(std::exception_ptr error, unsigned attempted_retries) const override;

    [[nodiscard]] unsigned get_max_retries() const noexcept override;
};
}

SEASTAR_MODULE_EXPORT_END
}
