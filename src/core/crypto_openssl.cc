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
#include <openssl/evp.h>
#include "../net/tls-impl.hh"
#include "../net/tls_openssl.hh"
#include <seastar/net/stack.hh>
#include <seastar/core/sstring.hh>
#include <seastar/util/assert.hh>
#include <seastar/util/defer.hh>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <cstring>
#include <stdexcept>
#include <seastar/core/internal/fmt.hh>

namespace seastar::internal::crypto {

class openssl_tls_backend final : public tls_backend {
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
    const char* name() const override { return "openssl"; }
};

class openssl_crypto_provider final : public crypto_provider {
public:
    sstring sha1_hash(std::string_view input) override;
    sstring base64_encode(std::string_view input) override;
    md5_hasher make_md5_hasher() override;
    tls_backend& get_tls_backend() override;
private:
    openssl_tls_backend _tls_backend;
};

sstring openssl_crypto_provider::sha1_hash(std::string_view input) {
    auto md_ptr = EVP_MD_fetch(nullptr, "SHA1", nullptr);
    if (!md_ptr) {
        throw std::runtime_error("Failed to fetch SHA-1 algorithm from OpenSSL");
    }
    auto free_evp_md_ptr = defer([&]() noexcept { EVP_MD_free(md_ptr); });

    unsigned char hash[20];
    unsigned int hash_size = sizeof(hash);
    SEASTAR_ASSERT(hash_size == static_cast<unsigned int>(EVP_MD_get_size(md_ptr)));

    if (1 != EVP_Digest(input.data(), input.size(), hash, &hash_size, md_ptr, nullptr)) {
        throw std::runtime_error("Failed to perform SHA-1 digest in OpenSSL");
    }

    return sstring(reinterpret_cast<const char*>(hash), sizeof(hash));
}

sstring openssl_crypto_provider::base64_encode(std::string_view input) {
    const auto encode_capacity = [](size_t input_size) {
        return (((4 * input_size) / 3) + 3) & ~0x3U;
    };
    auto base64_encoded = uninitialized_string<sstring>(encode_capacity(input.size()));
    EVP_EncodeBlock(reinterpret_cast<unsigned char *>(base64_encoded.data()), reinterpret_cast<const unsigned char *>(input.data()), input.size());
    return base64_encoded;
}

namespace {

static_assert(sizeof(EVP_MD_CTX*) <= md5_hasher::max_ctx_size,
    "EVP_MD_CTX* does not fit in md5_hasher inline storage");

void openssl_md5_update(unsigned char* ctx, const void* data, size_t len) {
    EVP_MD_CTX* mdctx;
    std::memcpy(&mdctx, ctx, sizeof(mdctx));
    if (1 != EVP_DigestUpdate(mdctx, data, len)) {
        throw std::runtime_error("OpenSSL EVP_DigestUpdate(MD5) failed");
    }
}

md5_hash openssl_md5_finalize(unsigned char* ctx) {
    EVP_MD_CTX* mdctx;
    std::memcpy(&mdctx, ctx, sizeof(mdctx));
    md5_hash result;
    unsigned int len = 0;
    if (1 != EVP_DigestFinal_ex(mdctx, result.data.data(), &len)) {
        EVP_MD_CTX_free(mdctx);
        std::memset(ctx, 0, sizeof(mdctx));
        throw std::runtime_error("OpenSSL EVP_DigestFinal_ex(MD5) failed");
    }
    EVP_MD_CTX_free(mdctx);
    std::memset(ctx, 0, sizeof(mdctx));
    return result;
}

void openssl_md5_destroy(unsigned char* ctx) {
    EVP_MD_CTX* mdctx;
    std::memcpy(&mdctx, ctx, sizeof(mdctx));
    if (mdctx) {
        EVP_MD_CTX_free(mdctx);
    }
}

const md5_hasher::ops openssl_md5_ops = {
    .update = openssl_md5_update,
    .finalize = openssl_md5_finalize,
    .destroy = openssl_md5_destroy,
};

} // anonymous namespace

md5_hasher openssl_crypto_provider::make_md5_hasher() {
    EVP_MD_CTX* mdctx = EVP_MD_CTX_new();
    if (!mdctx) {
        throw std::runtime_error("OpenSSL EVP_MD_CTX_new() failed");
    }
    if (1 != EVP_DigestInit_ex(mdctx, EVP_md5(), nullptr)) {
        EVP_MD_CTX_free(mdctx);
        throw std::runtime_error("OpenSSL EVP_DigestInit_ex(MD5) failed");
    }
    md5_hasher h(&openssl_md5_ops);
    std::memcpy(h.ctx(), &mdctx, sizeof(mdctx));
    return h;
}

shared_ptr<tls::session_impl> openssl_tls_backend::make_session(
        tls::session_type type,
        shared_ptr<tls::certificate_credentials> creds,
        std::unique_ptr<net::connected_socket_impl> sock,
        const tls::tls_options& options) {
    return tls::openssl::make_session(type, std::move(creds), std::move(sock), options);
}

const std::error_category& openssl_tls_backend::error_category() {
    return tls::openssl::error_category();
}

std::vector<uint8_t> openssl_tls_backend::generate_session_ticket_key() {
    return tls::openssl::generate_session_ticket_key();
}

shared_ptr<tls::credentials_impl> openssl_tls_backend::make_credentials_impl() {
    return tls::openssl::make_credentials_impl();
}

std::unique_ptr<tls::dh_params_impl> openssl_tls_backend::make_dh_params(tls::dh_params::level lvl) {
    return tls::openssl::make_dh_params(lvl);
}

std::unique_ptr<tls::dh_params_impl> openssl_tls_backend::make_dh_params(const tls::blob& b, tls::x509_crt_format fmt) {
    return tls::openssl::make_dh_params(b, fmt);
}

void openssl_tls_backend::init_error_codes() {
    tls::openssl::init_error_codes();
}

tls_backend& openssl_crypto_provider::get_tls_backend() {
    return _tls_backend;
}

std::unique_ptr<crypto_provider> create_openssl_provider() {
    return std::make_unique<openssl_crypto_provider>();
}

} // namespace seastar::internal::crypto
