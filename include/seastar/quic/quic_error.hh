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
 * Copyright (C) 2026 ScyllaDB Ltd.
 */

#pragma once

#include <cstdint>
#include <system_error>
#include <string>
#include <type_traits>

namespace seastar::quic::experimental {

/// Error raised by the experimental QUIC transport.
///
/// The numeric values are stable within this API and are exposed through
/// std::system_error::code() using quic_error_category().
class quic_error final : public std::system_error {
public:
    /// Transport-independent QUIC error classes.
    enum value : uint8_t {
        none = 0,
        invalid_argument,
        invalid_state,
        io,
        timeout,
        protocol,
        closed,
        unsupported,
        internal,
        backend,
    };

    /// Constructs a system_error carrying a QUIC error code and optional detail.
    explicit quic_error(value error, std::string detail = {});
};

/// Returns the std::error_category used by quic_error.
const std::error_category& quic_error_category() noexcept;

/// Converts a QUIC error enum to std::error_code.
std::error_code make_error_code(quic_error::value error) noexcept;

} // namespace seastar::quic::experimental

namespace std {

template <>
struct is_error_code_enum<seastar::quic::experimental::quic_error::value> : true_type {};

} // namespace std
