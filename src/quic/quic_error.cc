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

#include "quic_error_impl.hh"

#include <system_error>
#include <utility>

#include <ngtcp2/ngtcp2.h>

#include <gnutls/gnutls.h>

namespace seastar::quic::experimental {

const char* to_string(quic_error_code error) noexcept {
    switch (error) {
    case quic_error_code::none:
        return "none";
    case quic_error_code::invalid_argument:
        return "invalid_argument";
    case quic_error_code::invalid_state:
        return "invalid_state";
    case quic_error_code::io:
        return "io";
    case quic_error_code::timeout:
        return "timeout";
    case quic_error_code::protocol:
        return "protocol";
    case quic_error_code::closed:
        return "closed";
    case quic_error_code::unsupported:
        return "unsupported";
    case quic_error_code::internal:
        return "internal";
    case quic_error_code::backend:
        return "backend";
    }
    return "unknown";
}

namespace {

class quic_error_category_impl final : public std::error_category {
public:
    const char* name() const noexcept override {
        return "seastar.quic";
    }

    std::string message(int ev) const override {
        return to_string(static_cast<quic_error_code>(ev));
    }
};

std::system_error make_quic_system_error(quic_error_code error, std::string detail) {
    auto code = make_error_code(error);
    if (detail.empty()) {
        return std::system_error(code);
    }
    return std::system_error(code, std::move(detail));
}

} // namespace

const std::error_category& quic_error_category() noexcept {
    static const quic_error_category_impl category;
    return category;
}

std::error_code make_error_code(quic_error_code error) noexcept {
    return {static_cast<int>(error), quic_error_category()};
}

quic_error::quic_error(quic_error_code error, std::string detail)
    : std::system_error(make_quic_system_error(error, std::move(detail))) {
}

[[noreturn]] void throw_quic_error(quic_error_code error, std::string_view detail) {
    throw quic_error(error, std::string(detail));
}

quic_error_code classify_ngtcp2_error(int code) noexcept {
    if (code == 0) {
        return quic_error_code::none;
    }
    if (code == NGTCP2_ERR_DRAINING) {
        return quic_error_code::closed;
    }
    if (code == NGTCP2_ERR_IDLE_CLOSE) {
        return quic_error_code::timeout;
    }
    if (code == NGTCP2_ERR_WRITE_MORE) {
        return quic_error_code::none;
    }
    if (code == NGTCP2_ERR_INVALID_ARGUMENT) {
        return quic_error_code::invalid_argument;
    }
    if (code == NGTCP2_ERR_PROTO) {
        return quic_error_code::protocol;
    }
    return quic_error_code::backend;
}

quic_error_code classify_gnutls_error(int code) noexcept {
    if (code >= 0) {
        return quic_error_code::none;
    }
    if (code == GNUTLS_E_AGAIN || code == GNUTLS_E_INTERRUPTED) {
        return quic_error_code::io;
    }
#ifdef GNUTLS_E_TIMEDOUT
    if (code == GNUTLS_E_TIMEDOUT) {
        return quic_error_code::timeout;
    }
#endif
    return quic_error_code::backend;
}

bool ngtcp2_is_write_more(int code) noexcept {
    return code == NGTCP2_ERR_WRITE_MORE;
}

bool ngtcp2_is_draining(int code) noexcept {
    return code == NGTCP2_ERR_DRAINING;
}

bool ngtcp2_is_idle_close(int code) noexcept {
    return code == NGTCP2_ERR_IDLE_CLOSE;
}

std::string ngtcp2_error_message(int code) {
    return ngtcp2_strerror(code);
}

std::string gnutls_error_message(int code) {
    return gnutls_strerror(code);
}

} // namespace seastar::quic::experimental
