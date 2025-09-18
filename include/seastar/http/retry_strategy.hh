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
    [[nodiscard]] virtual unsigned get_max_retries() const noexcept = 0;

public:
    virtual ~retry_strategy() = default;
    // Returns true if the error can be retried given the error and the number of times already tried.
    virtual future<bool> should_retry(std::exception_ptr error, unsigned attempted_retries) const = 0;
};

class default_retry_strategy : public retry_strategy {
    [[nodiscard]] unsigned get_max_retries() const noexcept override;

    unsigned _max_retries;

public:
    default_retry_strategy();
    default_retry_strategy(unsigned max_retries);

    future<bool> should_retry(std::exception_ptr error, unsigned attempted_retries) const override;
};

class no_retry_strategy : public default_retry_strategy {
    [[nodiscard]] unsigned get_max_retries() const noexcept override;

public:
    future<bool> should_retry(std::exception_ptr error, unsigned attempted_retries) const override;
};
} // namespace http::experimental

SEASTAR_MODULE_EXPORT_END
} // namespace seastar
