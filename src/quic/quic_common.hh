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

#include <chrono>
#include <optional>
#include <span>
#include <string_view>

#include <ngtcp2/ngtcp2.h>

#include <seastar/core/temporary_buffer.hh>
#include <seastar/net/api.hh>
#include <seastar/quic/quic.hh>
#include <seastar/util/log.hh>

namespace seastar::quic::experimental {

void ensure_gnutls_global();
void rand_bytes_or_throw(uint8_t* dst, size_t len, const char* context);
bool rand_bytes_or_log(logger& log, std::string_view owner, uint8_t* dst, size_t len, const char* context) noexcept;

const ngtcp2_mem* ngtcp2_mem_for_thread();

void init_ngtcp2_addr(ngtcp2_addr* addr, const sockaddr* sa, size_t len);
void to_sockaddr_storage(const socket_address& sa, sockaddr_storage& out, socklen_t& outlen);
void validate_ip_socket_address(const socket_address& sa, std::string_view what);
std::optional<socket_address> to_socket_address(const ngtcp2_addr& addr);

ngtcp2_tstamp quic_now_ns() noexcept;
socket_address wildcard_address_for_family(sa_family_t family);
std::optional<congestion_control_algorithm> effective_congestion_control(const transport_config& cfg);
uint64_t effective_initial_receive_window(const connection_options& options, uint64_t configured) noexcept;
future<> send_datagram(logger& log, net::datagram_channel& channel, const socket_address& dst, temporary_buffer<char> packet);

temporary_buffer<char> linearize_packet(std::span<temporary_buffer<char>> bufs);
ngtcp2_cc_algo to_ngtcp2_cc_algo(congestion_control_algorithm algo);

} // namespace seastar::quic::experimental
