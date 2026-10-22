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

#include <seastar/core/sstring.hh>
#include <seastar/core/internal/md5.hh>
#include <cstdint>
#include <memory>
#include <string_view>
#include <system_error>
#include <vector>

#include <seastar/core/shared_ptr.hh>
#include <seastar/net/tls.hh>

namespace seastar::net { class connected_socket_impl; }
namespace seastar::tls {
    class session_impl;
    class credentials_impl;
    class dh_params_impl;
}

namespace seastar::internal::crypto {

/// \brief Abstract interface for a TLS backend.
///
/// Provides factory methods for creating TLS sessions and accessing
/// backend-specific error handling.  Retrieved via
/// crypto_provider::get_tls_backend().
class tls_backend {
public:
    virtual ~tls_backend() = default;

    /// \brief Create a TLS session wrapping the given socket.
    virtual shared_ptr<tls::session_impl> make_session(
        tls::session_type type,
        shared_ptr<tls::certificate_credentials> creds,
        std::unique_ptr<net::connected_socket_impl> sock,
        const tls::tls_options& options) = 0;

    /// \brief Return the backend-specific TLS error category.
    virtual const std::error_category& error_category() = 0;

    /// \brief Generate a session ticket encryption key.
    virtual std::vector<uint8_t> generate_session_ticket_key() = 0;

    /// \brief Create a backend-specific credentials implementation.
    virtual shared_ptr<tls::credentials_impl> make_credentials_impl() = 0;

    /// \brief Create backend-specific DH parameters from a security level.
    virtual std::unique_ptr<tls::dh_params_impl> make_dh_params(tls::dh_params::level) = 0;

    /// \brief Create backend-specific DH parameters from raw data.
    virtual std::unique_ptr<tls::dh_params_impl> make_dh_params(const tls::blob&, tls::x509_crt_format) = 0;

    /// \brief Initialize backend-specific TLS error code constants.
    ///
    /// In dual-backend builds this fills in the legacy \c tls::ERROR_*
    /// globals from the active backend's values. In single-backend builds
    /// the globals are \c const, statically initialized in the backend's
    /// own translation unit, and this method is a no-op.
    virtual void init_error_codes() = 0;

    /// \brief Return the name of this TLS backend (e.g. "gnutls", "openssl").
    virtual const char* name() const = 0;
};

/// \brief Abstract interface for cryptographic primitives.
///
/// Provides backend-agnostic access to hashing and encoding
/// operations. Implementations exist for GnuTLS and (in the future)
/// OpenSSL.  A process-wide provider is selected at startup via
/// set_provider() and retrieved with provider().
class crypto_provider {
public:
    virtual ~crypto_provider() = default;

    /// \brief Compute the SHA-1 hash of \p input.
    /// \return The raw 20-byte hash value.
    virtual sstring sha1_hash(std::string_view input) = 0;

    /// \brief Base64-encode \p input.
    /// \return The base64-encoded string.
    virtual sstring base64_encode(std::string_view input) = 0;

    /// \brief Create an incremental MD5 hasher.
    virtual md5_hasher make_md5_hasher() = 0;

    /// \brief Return the TLS backend for this crypto provider.
    virtual tls_backend& get_tls_backend() = 0;
};

/// \brief Return the process-wide crypto provider.
///
/// In dual-backend builds, must be called after \ref set_provider(). The
/// returned reference remains valid for the lifetime of the process.
/// In single-backend builds the provider is fixed at compile time, lazily
/// created on first call, and \ref provider() works at any time including
/// from static initializers / before reactor startup.
crypto_provider& provider();

#ifdef SEASTAR_TLS_DUAL_BACKEND
/// \brief Install the process-wide crypto provider.
///
/// Must be called exactly once per \c set_provider / \c reset_provider
/// cycle, before any call to \ref provider(). Ownership is transferred
/// to the crypto subsystem.
///
/// Only compiled in dual-backend builds. In single-backend builds the
/// provider is fixed at compile time and \ref provider() handles the
/// lifetime internally.
void set_provider(std::unique_ptr<crypto_provider> p);

/// \brief Tear down the process-wide crypto provider installed by
/// \ref set_provider.
///
/// Called from \c smp::cleanup() so that a subsequent \c app::run()
/// (and the \c smp::configure() it triggers) starts from a clean slate
/// and can call \ref set_provider again. Safe to call when no provider
/// is installed.
///
/// Only compiled in dual-backend builds.
void reset_provider();
#endif

#ifdef SEASTAR_HAVE_GNUTLS
/// \brief Create a GnuTLS-backed crypto provider.
std::unique_ptr<crypto_provider> create_gnutls_provider();
#endif

#ifdef SEASTAR_HAVE_OPENSSL
/// \brief Create an OpenSSL-backed crypto provider.
std::unique_ptr<crypto_provider> create_openssl_provider();
#endif

} // namespace seastar::internal::crypto
