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

namespace seastar {

namespace net {

struct ipv4_address {
    ipv4_address() noexcept : ip(0) {}
    explicit ipv4_address(uint32_t ip) noexcept : ip(ip) {}
    // throws if addr is not a valid ipv4 address
    explicit ipv4_address(const std::string& addr);
    ipv4_address(ipv4_addr addr) noexcept {
        ip = addr.ip;
    }

    packed<uint32_t> ip;

    template <typename Adjuster>
    auto adjust_endianness(Adjuster a) { return a(ip); }

    friend bool operator==(ipv4_address x, ipv4_address y) noexcept {
        return x.ip == y.ip;
    }
    friend bool operator!=(ipv4_address x, ipv4_address y) noexcept {
        return x.ip != y.ip;
    }

    static ipv4_address read(const char* p) noexcept {
        ipv4_address ia;
        ia.ip = read_be<uint32_t>(p);
        return ia;
    }
    static ipv4_address consume(const char*& p) noexcept {
        auto ia = read(p);
        p += 4;
        return ia;
    }
    void write(char* p) const noexcept {
        write_be<uint32_t>(p, ip);
    }
    void produce(char*& p) const noexcept {
        produce_be<uint32_t>(p, ip);
    }
    static constexpr size_t size() {
        return 4;
    }
} __attribute__((packed));

static inline bool is_unspecified(ipv4_address addr) noexcept { return addr.ip == 0; }

std::ostream& operator<<(std::ostream& os, const ipv4_address& a);

}

}

namespace std {

template <>
struct hash<seastar::net::ipv4_address> {
    size_t operator()(seastar::net::ipv4_address a) const { return a.ip; }
};

}
