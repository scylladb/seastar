/*
 * Copyright (C) 2025-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: LicenseRef-ScyllaDB-Source-Available-1.0
 */

#pragma once
#include <seastar/core/future.hh>
#include <seastar/http/reply.hh>

namespace seastar {

namespace http::experimental {

class retry_strategy {
public:
    virtual ~retry_strategy() = default;
    // Returns true if the error can be retried given the error and the number of times already tried.
    virtual future<bool> should_retry(std::exception_ptr error, unsigned attempted_retries) const = 0;
};

class default_retry_strategy final : public retry_strategy {
    unsigned _max_retries;

public:
    default_retry_strategy();
    explicit default_retry_strategy(unsigned max_retries);

    future<bool> should_retry(std::exception_ptr error, unsigned attempted_retries) const override;
};

class no_retry_strategy final : public retry_strategy {
public:
    future<bool> should_retry(std::exception_ptr error, unsigned attempted_retries) const override;
};
} // namespace http::experimental

} // namespace seastar
