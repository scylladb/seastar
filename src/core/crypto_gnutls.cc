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
#include "../net/tls-impl.hh"
#include "../net/tls_gnutls.hh"
#include <seastar/net/stack.hh>
#include <seastar/util/defer.hh>
#include <gnutls/crypto.h>
#include <gnutls/gnutls.h>
#include <cstring>
#include <stdexcept>
#include <seastar/core/internal/fmt.hh>

namespace seastar::internal::crypto {

class gnutls_tls_backend final : public tls_backend {
public:
    shared_ptr<tls::session_impl> make_session(
            tls::session_type type,
            shared_ptr<tls::certificate_credentials> creds,
            std::unique_ptr<net::connected_socket_impl> sock,
            const tls::tls_options& options) override;
    const std::error_category& error_category() override;
    std::vector<uint8_t> generate_session_ticket_key() override;
    shared_ptr<tls::credentials_impl> make_credentials_impl() override;
    std::unique_ptr<tls::dh_params_impl> make_dh_params(tls::dh_params::level) override;
    std::unique_ptr<tls::dh_params_impl> make_dh_params(const tls::blob&, tls::x509_crt_format) override;
    void init_error_codes() override;
    const char* name() const override { return "gnutls"; }
};

class gnutls_crypto_provider final : public crypto_provider {
public:
    sstring sha1_hash(std::string_view input) override;
    sstring base64_encode(std::string_view input) override;
    md5_hasher make_md5_hasher() override;
    tls_backend& get_tls_backend() override;
private:
    gnutls_tls_backend _tls_backend;
};

sstring gnutls_crypto_provider::sha1_hash(std::string_view input) {
    unsigned char hash[20];
    if (int ret = gnutls_hash_fast(GNUTLS_DIG_SHA1, input.data(), input.size(), hash);
        ret != GNUTLS_E_SUCCESS) {
        throw std::runtime_error(fmt::format("gnutls_hash_fast: {}", gnutls_strerror(ret)));
    }
    return sstring(reinterpret_cast<const char*>(hash), sizeof(hash));
}

sstring gnutls_crypto_provider::base64_encode(std::string_view input) {
    gnutls_datum_t src_data{
        .data = reinterpret_cast<uint8_t*>(const_cast<char*>(input.data())),
        .size = static_cast<unsigned>(input.size())
    };
    gnutls_datum_t encoded_data;
    if (int ret = gnutls_base64_encode2(&src_data, &encoded_data); ret != GNUTLS_E_SUCCESS) {
        throw std::runtime_error(fmt::format("gnutls_base64_encode2: {}", gnutls_strerror(ret)));
    }
    auto free_encoded_data = defer([&] () noexcept { gnutls_free(encoded_data.data); });
    return sstring(reinterpret_cast<const char*>(encoded_data.data), encoded_data.size);
}

namespace {

static_assert(sizeof(gnutls_hash_hd_t) <= md5_hasher::max_ctx_size,
    "gnutls_hash_hd_t does not fit in md5_hasher inline storage");

void gnutls_md5_update(unsigned char* ctx, const void* data, size_t len) {
    gnutls_hash_hd_t handle;
    std::memcpy(&handle, ctx, sizeof(handle));
    gnutls_hash(handle, data, len);
}

md5_hash gnutls_md5_finalize(unsigned char* ctx) {
    gnutls_hash_hd_t handle;
    std::memcpy(&handle, ctx, sizeof(handle));
    md5_hash result;
    gnutls_hash_deinit(handle, result.data.data());
    std::memset(ctx, 0, sizeof(handle));
    return result;
}

void gnutls_md5_destroy(unsigned char* ctx) {
    gnutls_hash_hd_t handle;
    std::memcpy(&handle, ctx, sizeof(handle));
    if (handle) {
        gnutls_hash_deinit(handle, nullptr);
    }
}

const md5_hasher::ops gnutls_md5_ops = {
    .update = gnutls_md5_update,
    .finalize = gnutls_md5_finalize,
    .destroy = gnutls_md5_destroy,
};

} // anonymous namespace

md5_hasher gnutls_crypto_provider::make_md5_hasher() {
    md5_hasher h(&gnutls_md5_ops);
    gnutls_hash_hd_t handle;
    if (int ret = gnutls_hash_init(&handle, GNUTLS_DIG_MD5); ret != GNUTLS_E_SUCCESS) {
        throw std::runtime_error(fmt::format("gnutls_hash_init(MD5): {}", gnutls_strerror(ret)));
    }
    std::memcpy(h.ctx(), &handle, sizeof(handle));
    return h;
}

shared_ptr<tls::session_impl> gnutls_tls_backend::make_session(
        tls::session_type type,
        shared_ptr<tls::certificate_credentials> creds,
        std::unique_ptr<net::connected_socket_impl> sock,
        const tls::tls_options& options) {
    return tls::gnutls::make_session(type, std::move(creds), std::move(sock), options);
}

const std::error_category& gnutls_tls_backend::error_category() {
    return tls::gnutls::error_category();
}

std::vector<uint8_t> gnutls_tls_backend::generate_session_ticket_key() {
    return tls::gnutls::generate_session_ticket_key();
}

shared_ptr<tls::credentials_impl> gnutls_tls_backend::make_credentials_impl() {
    return tls::gnutls::make_credentials_impl();
}

std::unique_ptr<tls::dh_params_impl> gnutls_tls_backend::make_dh_params(tls::dh_params::level lvl) {
    return tls::gnutls::make_dh_params(lvl);
}

std::unique_ptr<tls::dh_params_impl> gnutls_tls_backend::make_dh_params(const tls::blob& b, tls::x509_crt_format fmt) {
    return tls::gnutls::make_dh_params(b, fmt);
}

void gnutls_tls_backend::init_error_codes() {
    tls::gnutls::init_error_codes();
}

tls_backend& gnutls_crypto_provider::get_tls_backend() {
    return _tls_backend;
}

std::unique_ptr<crypto_provider> create_gnutls_provider() {
    return std::make_unique<gnutls_crypto_provider>();
}

} // namespace seastar::internal::crypto
