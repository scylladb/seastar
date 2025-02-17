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
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 */

#pragma once

#if FMT_VERSION >= 90000
#include <fmt/ostream.h>
#endif

#include <array>
#include <algorithm>
#include <seastar/net/byteorder.hh>
#include <seastar/util/assert.hh>

namespace seastar {

namespace net {

struct ethernet_address {
    ethernet_address() noexcept
        : mac{} {}

    ethernet_address(const uint8_t *eaddr) noexcept {
        std::copy(eaddr, eaddr + 6, mac.begin());
    }

    ethernet_address(std::initializer_list<uint8_t> eaddr) noexcept {
        SEASTAR_ASSERT(eaddr.size() == mac.size());
        std::copy(eaddr.begin(), eaddr.end(), mac.begin());
    }

    std::array<uint8_t, 6> mac;

    template <typename Adjuster>
    void adjust_endianness(Adjuster) noexcept {}

    static ethernet_address read(const char* p) noexcept {
        ethernet_address ea;
        std::copy_n(p, size(), reinterpret_cast<char*>(ea.mac.data()));\
        return ea;
    }
    static ethernet_address consume(const char*& p) noexcept {
        auto ea = read(p);
        p += size();
        return ea;
    }
    void write(char* p) const noexcept {
        std::copy_n(reinterpret_cast<const char*>(mac.data()), size(), p);
    }
    void produce(char*& p) const noexcept {
        write(p);
        p += size();
    }
    static constexpr size_t size() noexcept {
        return 6;
    }
} __attribute__((packed));

std::ostream& operator<<(std::ostream& os, ethernet_address ea);

struct ethernet {
    using address = ethernet_address;
    static address broadcast_address() {
        return  {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    }
    static constexpr uint16_t arp_hardware_type() { return 1; }
};

struct eth_hdr {
    ethernet_address dst_mac;
    ethernet_address src_mac;
    packed<uint16_t> eth_proto;
    template <typename Adjuster>
    auto adjust_endianness(Adjuster a) {
        return a(eth_proto);
    }
} __attribute__((packed));

ethernet_address parse_ethernet_address(std::string addr);
}

}

#if FMT_VERSION >= 90000
template <> struct fmt::formatter<seastar::net::ethernet_address> : fmt::ostream_formatter {};
#endif
