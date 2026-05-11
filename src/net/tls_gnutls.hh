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
 * Copyright 2024 ScyllaDB
 */
#pragma once

#include <cstdint>
#include <memory>
#include <system_error>
#include <vector>

#include <seastar/core/shared_ptr.hh>

namespace seastar::net { class connected_socket_impl; }
namespace seastar::tls {
    class session_impl;
    class credentials_impl;
    class dh_params_impl;
    class dh_params;
    class certificate_credentials;
    enum class session_type;
    enum class x509_crt_format;
    struct tls_options;
    typedef std::basic_string_view<char> blob;
}

namespace seastar::tls::gnutls {

/// Create a GnuTLS TLS session.
shared_ptr<session_impl> make_session(
    session_type type,
    shared_ptr<certificate_credentials> creds,
    std::unique_ptr<net::connected_socket_impl> sock,
    const tls_options& options);

/// Return the GnuTLS error category.
const std::error_category& error_category();

/// Generate a session ticket key using GnuTLS.
std::vector<uint8_t> generate_session_ticket_key();

/// Create a GnuTLS credentials implementation.
shared_ptr<credentials_impl> make_credentials_impl();

/// Create GnuTLS DH parameters from a security level.
std::unique_ptr<dh_params_impl> make_dh_params(dh_params::level);

/// Create GnuTLS DH parameters from raw data.
std::unique_ptr<dh_params_impl> make_dh_params(const blob&, x509_crt_format);

/// Initialize TLS error codes with GnuTLS values.
///
/// In dual-backend builds (\c SEASTAR_TLS_DUAL_BACKEND) the legacy
/// \c seastar::tls::ERROR_* globals are zero at static-init time and this
/// function fills them in at reactor startup. In single-backend builds the
/// globals are statically initialized to their GnuTLS values and this is a
/// no-op.
void init_error_codes();

} // namespace seastar::tls::gnutls
