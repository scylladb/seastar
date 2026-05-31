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

#include "quic_common.hh"

#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>

#include <gnutls/crypto.h>
#include <gnutls/gnutls.h>

#include "quic_error_impl.hh"

namespace seastar::quic::experimental {

namespace {

class gnutls_global_guard {
public:
    gnutls_global_guard() {
        gnutls_global_init();
    }

    ~gnutls_global_guard() {
        gnutls_global_deinit();
    }
};

int try_rand_bytes(uint8_t* dst, size_t len) noexcept {
    return gnutls_rnd(GNUTLS_RND_RANDOM, dst, len);
}

[[noreturn]] void throw_random_failure(const char* context, int rv) {
    throw quic_error(
      classify_gnutls_error(rv),
      sstring(context) + ": " + gnutls_error_message(rv));
}

void* mem_malloc(size_t size, void*) {
    return std::malloc(size);
}

void mem_free(void* ptr, void*) {
    std::free(ptr);
}

void* mem_calloc(size_t n, size_t s, void*) {
    return std::calloc(n, s);
}

void* mem_realloc(void* ptr, size_t s, void*) {
    return std::realloc(ptr, s);
}

} // namespace

void ensure_gnutls_global() {
    static gnutls_global_guard guard;
}

void rand_bytes_or_throw(uint8_t* dst, size_t len, const char* context) {
    auto rv = try_rand_bytes(dst, len);
    if (rv < 0) {
        throw_random_failure(context, rv);
    }
}

bool rand_bytes_or_log(logger& log, std::string_view owner, uint8_t* dst, size_t len, const char* context) noexcept {
    auto rv = try_rand_bytes(dst, len);
    if (rv >= 0) {
        return true;
    }
    log.error("{} random source failure: context={} detail='{}'", owner, context, gnutls_error_message(rv));
    return false;
}

const ngtcp2_mem* ngtcp2_mem_for_thread() {
    thread_local const ngtcp2_mem mem = {
      nullptr,
      mem_malloc,
      mem_free,
      mem_calloc,
      mem_realloc,
    };
    return &mem;
}

void init_ngtcp2_addr(ngtcp2_addr* addr, const sockaddr* sa, size_t len) {
    addr->addr = const_cast<sockaddr*>(sa);
    addr->addrlen = static_cast<socklen_t>(len);
}

void to_sockaddr_storage(const socket_address& sa, sockaddr_storage& out, socklen_t& outlen) {
    std::memset(&out, 0, sizeof(out));
    switch (sa.family()) {
    case AF_INET: {
        auto in = sa.as_posix_sockaddr_in();
        std::memcpy(&out, &in, sizeof(in));
        outlen = sizeof(in);
        return;
    }
    case AF_INET6: {
        auto in6 = sa.as_posix_sockaddr_in6();
        std::memcpy(&out, &in6, sizeof(in6));
        outlen = sizeof(in6);
        return;
    }
    default:
        throw_quic_error(quic_error_code::invalid_argument, "QUIC requires an IPv4 or IPv6 socket address");
    }
}

void validate_ip_socket_address(const socket_address& sa, std::string_view what) {
    switch (sa.family()) {
    case AF_INET:
    case AF_INET6:
        return;
    default:
        throw_quic_error(
          quic_error_code::invalid_argument,
          sstring(what) + " must be an IPv4 or IPv6 socket address");
    }
}

std::optional<socket_address> to_socket_address(const ngtcp2_addr& addr) {
    if (!addr.addr || addr.addrlen == 0) {
        return std::nullopt;
    }

    auto* sa = reinterpret_cast<const sockaddr*>(addr.addr);
    switch (sa->sa_family) {
    case AF_INET: {
        if (addr.addrlen < sizeof(sockaddr_in)) {
            return std::nullopt;
        }
        sockaddr_in in{};
        std::memcpy(&in, sa, sizeof(in));
        return socket_address(in);
    }
    case AF_INET6: {
        if (addr.addrlen < sizeof(sockaddr_in6)) {
            return std::nullopt;
        }
        sockaddr_in6 in6{};
        std::memcpy(&in6, sa, sizeof(in6));
        return socket_address(in6);
    }
    default:
        return std::nullopt;
    }
}

ngtcp2_tstamp quic_now_ns() noexcept {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

socket_address wildcard_address_for_family(sa_family_t family) {
    switch (family) {
    case AF_INET: {
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        return socket_address(sa);
    }
    case AF_INET6: {
        sockaddr_in6 sa{};
        sa.sin6_family = AF_INET6;
        return socket_address(sa);
    }
    default:
        throw_quic_error(quic_error_code::invalid_argument, "QUIC requires an IPv4 or IPv6 socket address");
    }
}

std::optional<congestion_control_algorithm> effective_congestion_control(const transport_config& cfg) {
    if (!cfg.congestion_control) {
        return std::nullopt;
    }
    auto algo = *cfg.congestion_control;
    // ngtcp2 BBR regresses sharply on the small-datagram benchmark profile.
    if ((algo == congestion_control_algorithm::bbr || algo == congestion_control_algorithm::bbr2)
        && cfg.max_tx_udp_payload_size
        && *cfg.max_tx_udp_payload_size <= 4096) {
        return congestion_control_algorithm::cubic;
    }
    return algo;
}

uint64_t effective_initial_receive_window(const connection_options& options, uint64_t configured) noexcept {
    if (!options.max_pending_receive_bytes) {
        return configured;
    }
    auto limit = static_cast<uint64_t>(options.max_pending_receive_bytes);
    return configured < limit ? configured : limit;
}

future<> send_datagram(logger& log, net::datagram_channel& channel, const socket_address& dst, temporary_buffer<char> packet) {
    if (packet.empty()) {
        co_return;
    }
    log.trace("udp send datagram: dst={} bytes={}", dst, packet.size());
    std::array<temporary_buffer<char>, 1> bufs{std::move(packet)};
    co_await channel.send(dst, std::span<temporary_buffer<char>>(bufs));
}

temporary_buffer<char> linearize_packet(std::span<temporary_buffer<char>> bufs) {
    if (bufs.empty()) {
        return temporary_buffer<char>();
    }
    if (bufs.size() == 1) {
        return std::move(bufs.front());
    }

    size_t total = 0;
    for (const auto& b : bufs) {
        total += b.size();
    }

    temporary_buffer<char> result(total);
    char* dst = result.get_write();
    size_t offset = 0;
    for (const auto& b : bufs) {
        std::memcpy(dst + offset, b.get(), b.size());
        offset += b.size();
    }
    return result;
}

ngtcp2_cc_algo to_ngtcp2_cc_algo(congestion_control_algorithm algo) {
    switch (algo) {
    case congestion_control_algorithm::reno:
        return NGTCP2_CC_ALGO_RENO;
    case congestion_control_algorithm::cubic:
        return NGTCP2_CC_ALGO_CUBIC;
    case congestion_control_algorithm::bbr:
        return NGTCP2_CC_ALGO_BBR;
    case congestion_control_algorithm::bbr2:
#ifdef NGTCP2_CC_ALGO_BBR2
        return NGTCP2_CC_ALGO_BBR2;
#else
        return NGTCP2_CC_ALGO_BBR;
#endif
    }
    return NGTCP2_CC_ALGO_CUBIC;
}

} // namespace seastar::quic::experimental
