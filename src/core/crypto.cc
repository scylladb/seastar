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

#include "crypto.hh"
#include <seastar/util/assert.hh>
#include <memory>

namespace seastar::internal::crypto {

#ifdef SEASTAR_TLS_DUAL_BACKEND

// Dual-backend build: the active provider is selected at reactor startup
// (--crypto-provider) and installed via set_provider() from smp::configure().
static std::unique_ptr<crypto_provider> the_provider;

crypto_provider& provider() {
    SEASTAR_DEBUG_ASSERT(the_provider != nullptr);
    return *the_provider;
}

void set_provider(std::unique_ptr<crypto_provider> p) {
    SEASTAR_ASSERT(the_provider == nullptr);
    the_provider = std::move(p);
    provider().get_tls_backend().init_error_codes();
}

void reset_provider() {
    the_provider.reset();
}

#else // single-backend

// Single-backend build: the provider is fixed at compile time. Use a
// function-local static so provider() works at any time, including before
// reactor startup. No set_provider() is compiled or needed.
crypto_provider& provider() {
#ifdef SEASTAR_HAVE_GNUTLS
    static auto instance = create_gnutls_provider();
#else // SEASTAR_HAVE_OPENSSL
    static auto instance = create_openssl_provider();
#endif
    return *instance;
}

#endif // SEASTAR_TLS_DUAL_BACKEND

md5_hasher make_md5_hasher() {
    return provider().make_md5_hasher();
}

} // namespace seastar::internal::crypto
