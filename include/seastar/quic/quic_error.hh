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

/// \brief Error reporting for the experimental QUIC transport.
namespace seastar::quic::experimental {

/// \brief Error raised by the experimental QUIC transport.
///
/// The numeric values are stable within this API and are exposed through
/// std::system_error::code() using quic_error_category().
class quic_error final : public std::system_error {
public:
    /// \brief Transport-independent QUIC error classes.
    enum value : uint8_t {
        none = 0, ///< No error.
        invalid_argument, ///< Invalid address, option, or protocol parameter.
        invalid_state, ///< Operation is not valid in the object's current state.
        io, ///< UDP or stream input/output failure.
        timeout, ///< Handshake, idle, or transport timeout.
        protocol, ///< QUIC protocol violation reported by ngtcp2.
        closed, ///< Stream, connection, client, or server has closed.
        unsupported, ///< Requested operation is unavailable on this platform.
        internal, ///< Internal consistency or resource failure.
        backend, ///< Error reported by ngtcp2, GnuTLS, or another backend.
    };

    /// \brief Construct an exception carrying a QUIC error code and optional context.
    ///
    /// \param error Transport-independent error class.
    /// \param detail Additional diagnostic context appended to what().
    explicit quic_error(value error, std::string detail = {});
};

/// \brief Get the error category shared by all QUIC error codes.
///
/// \return A process-lifetime error category named `seastar.quic`.
const std::error_category& quic_error_category() noexcept;

/// \brief Convert a QUIC error class to std::error_code.
///
/// \param error Transport-independent error class.
/// \return An error code in quic_error_category().
std::error_code make_error_code(quic_error::value error) noexcept;

} // namespace seastar::quic::experimental

namespace std {

/// \brief Enable implicit construction of std::error_code from QUIC error values.
template <>
struct is_error_code_enum<seastar::quic::experimental::quic_error::value> : true_type {};

} // namespace std
