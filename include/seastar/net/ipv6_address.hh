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
 * Copyright (C) 2022 ScyllaDB
 *
 */

#pragma once

#include <seastar/net/socket_defs.hh>
#include <seastar/core/byteorder.hh>

#include <array>

namespace seastar {

namespace net {

struct ipv6_address {
    using ipv6_bytes = std::array<uint8_t, 16>;

    static_assert(alignof(ipv6_bytes) == 1, "ipv6_bytes should be byte-aligned");
    static_assert(sizeof(ipv6_bytes) == 16, "ipv6_bytes should be 16 bytes");

    ipv6_address() noexcept;
    explicit ipv6_address(const ::in6_addr&) noexcept;
    explicit ipv6_address(const ipv6_bytes&) noexcept;
    // throws if addr is not a valid ipv6 address
    explicit ipv6_address(const std::string&);
    ipv6_address(const ipv6_addr& addr) noexcept;

    // No need to use packed - we only store
    // as byte array. If we want to read as
    // uints or whatnot, we must copy
    ipv6_bytes ip;

    template <typename Adjuster>
    auto adjust_endianness(Adjuster a) { return a(ip); }

    bool operator==(const ipv6_address& y) const noexcept {
        return bytes() == y.bytes();
    }
    bool operator!=(const ipv6_address& y) const noexcept {
        return !(*this == y);
    }

    const ipv6_bytes& bytes() const noexcept {
        return ip;
    }

    bool is_unspecified() const noexcept;

    static ipv6_address read(const char*) noexcept;
    static ipv6_address consume(const char*& p) noexcept;
    void write(char* p) const noexcept;
    void produce(char*& p) const noexcept;
    static constexpr size_t size() {
        return sizeof(ipv6_bytes);
    }
} __attribute__((packed));

std::ostream& operator<<(std::ostream&, const ipv6_address&);

}

}

namespace std {

template <>
struct hash<seastar::net::ipv6_address> {
    size_t operator()(const seastar::net::ipv6_address&) const;
};

}
