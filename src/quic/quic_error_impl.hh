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

#include <string>
#include <string_view>

#include <seastar/quic/quic_error.hh>

namespace seastar::quic::experimental {

using quic_error_code = quic_error::value;

const char* to_string(quic_error_code error) noexcept;

[[noreturn]] void throw_quic_error(quic_error_code error, std::string_view detail = {});

quic_error_code classify_ngtcp2_error(int code) noexcept;
quic_error_code classify_gnutls_error(int code) noexcept;

bool ngtcp2_is_write_more(int code) noexcept;
bool ngtcp2_is_draining(int code) noexcept;
bool ngtcp2_is_idle_close(int code) noexcept;

std::string ngtcp2_error_message(int code);
std::string gnutls_error_message(int code);

} // namespace seastar::quic::experimental
