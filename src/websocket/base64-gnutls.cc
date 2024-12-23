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

#include <seastar/util/defer.hh>
#include <seastar/websocket/server.hh>

#include "websocket/base64.hh"

#include <gnutls/crypto.h>
#include <gnutls/gnutls.h>

namespace seastar::experimental::websocket {
std::string sha1_base64(std::string_view source) {
    unsigned char hash[20];
    assert(sizeof(hash) == gnutls_hash_get_len(GNUTLS_DIG_SHA1));
    if (int ret = gnutls_hash_fast(GNUTLS_DIG_SHA1, source.data(), source.size(), hash);
        ret != GNUTLS_E_SUCCESS) {
        throw websocket::exception(fmt::format("gnutls_hash_fast: {}", gnutls_strerror(ret)));
    }
    gnutls_datum_t hash_data{
        .data = hash,
        .size = sizeof(hash),
    };
    gnutls_datum_t base64_encoded;
    if (int ret = gnutls_base64_encode2(&hash_data, &base64_encoded);
        ret != GNUTLS_E_SUCCESS) {
        throw websocket::exception(fmt::format("gnutls_base64_encode2: {}", gnutls_strerror(ret)));
    }
    auto free_base64_encoded = defer([&] () noexcept { gnutls_free(base64_encoded.data); });
    // base64_encoded.data is "unsigned char *"
    return std::string(reinterpret_cast<const char*>(base64_encoded.data), base64_encoded.size);
}
}
