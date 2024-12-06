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
 * Copyright 2015 Cloudius Systems
 */

#include <seastar/core/sstring.hh>
#include <seastar/util/defer.hh>
#include <seastar/websocket/server.hh>

#include "websocket/base64.hh"

#include <openssl/evp.h>

namespace seastar::experimental::websocket {

std::string sha1_base64(std::string_view source) {
    unsigned char hash[20];

    const auto encode_capacity = [](size_t input_size) {
        return (((4 * input_size) / 3) + 3) & ~0x3U;
    };
    unsigned int hash_size = sizeof(hash);
    auto md_ptr = EVP_MD_fetch(nullptr, "SHA1", nullptr);
    if (!md_ptr) {
        throw websocket::exception("Failed to fetch SHA-1 algorithm from OpenSSL");
    }

    auto free_evp_md_ptr =
        defer([&]() noexcept  { EVP_MD_free(md_ptr); });

    assert(hash_size == static_cast<unsigned int>(EVP_MD_get_size(md_ptr)));

    if (1 != EVP_Digest(source.data(), source.size(), hash, &hash_size, md_ptr, nullptr)) {
        throw websocket::exception("Failed to perform SHA-1 digest in OpenSSL");
    }

    auto base64_encoded = uninitialized_string<std::string>(encode_capacity(hash_size));
    EVP_EncodeBlock(reinterpret_cast<unsigned char *>(base64_encoded.data()), hash, hash_size);
    return base64_encoded;
}

}