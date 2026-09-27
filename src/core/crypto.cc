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
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <fmt/format.h>

namespace seastar::internal::crypto {

// Never replaced once installed: other seastar instances in the same
// process may be using it concurrently.
static std::unique_ptr<crypto_provider> the_provider;
static std::mutex the_provider_mutex;

crypto_provider& provider() {
    return *the_provider;
}

void set_provider(std::unique_ptr<crypto_provider> p) {
    std::lock_guard lock(the_provider_mutex);
    if (the_provider) {
        auto installed = the_provider->get_tls_backend().name();
        auto requested = p->get_tls_backend().name();
        if (std::strcmp(installed, requested) != 0) {
            throw std::runtime_error(fmt::format("cannot use crypto provider {}: provider {} is already in use in this process",
                    requested, installed));
        }
        return;
    }
    the_provider = std::move(p);
    provider().get_tls_backend().init_error_codes();
}

md5_hasher make_md5_hasher() {
    return provider().make_md5_hasher();
}

} // namespace seastar::internal::crypto
