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
 * Copyright (C) 2016 ScyllaDB.
 */
#pragma once

#ifndef SEASTAR_MODULE
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/ip.h>
#include <array>
#include <cassert>
#include <functional>
#include <iosfwd>
#include <fmt/ostream.h>
#endif
#include <seastar/net/byteorder.hh>
#include <seastar/net/unix_address.hh>

namespace seastar {

SEASTAR_MODULE_EXPORT_BEGIN

namespace net {
class inet_address;
}

struct ipv4_addr;
struct ipv6_addr;

class socket_address {
public:
    socklen_t addr_length; ///!< actual size of the relevant 'u' member
    union {
        ::sockaddr_storage sas;
        ::sockaddr sa;
        ::sockaddr_in in;
        ::sockaddr_in6 in6;
        ::sockaddr_un un;
    } u;
    socket_address(const sockaddr_in& sa) noexcept : addr_length{sizeof(::sockaddr_in)} {
        u.in = sa;
    }
    socket_address(const sockaddr_in6& sa) noexcept : addr_length{sizeof(::sockaddr_in6)} {
        u.in6 = sa;
    }
    socket_address(uint16_t) noexcept;
    socket_address(ipv4_addr) noexcept;
    socket_address(const ipv6_addr&) noexcept;
    socket_address(const ipv6_addr&, uint32_t scope) noexcept;
    socket_address(const net::inet_address&, uint16_t p = 0) noexcept;
    explicit socket_address(const unix_domain_addr&) noexcept;
    /** creates an uninitialized socket_address. this can be written into, or used as
     *  "unspecified" for such addresses as bind(addr) or local address in socket::connect
     *  (i.e. system picks)
     */
    socket_address() noexcept;

    ::sockaddr& as_posix_sockaddr() noexcept { return u.sa; }
    ::sockaddr_in& as_posix_sockaddr_in() noexcept { return u.in; }
    ::sockaddr_in6& as_posix_sockaddr_in6() noexcept { return u.in6; }
    const ::sockaddr& as_posix_sockaddr() const noexcept { return u.sa; }
    const ::sockaddr_in& as_posix_sockaddr_in() const noexcept { return u.in; }
    const ::sockaddr_in6& as_posix_sockaddr_in6() const noexcept { return u.in6; }

    socket_address(uint32_t, uint16_t p = 0) noexcept;

    socklen_t length() const noexcept { return addr_length; };

    bool is_af_unix() const noexcept {
        return u.sa.sa_family == AF_UNIX;
    }

    bool is_unspecified() const noexcept;

    sa_family_t family() const noexcept {
        return u.sa.sa_family;
    }

    net::inet_address addr() const noexcept;
    ::in_port_t port() const noexcept;
    bool is_wildcard() const noexcept;

    bool operator==(const socket_address&) const noexcept;
    bool operator!=(const socket_address& a) const noexcept {
        return !(*this == a);
    }
};

std::ostream& operator<<(std::ostream&, const socket_address&);

enum class transport {
    TCP = IPPROTO_TCP,
    SCTP = IPPROTO_SCTP
};

struct ipv4_addr {
    uint32_t ip;
    uint16_t port;

    ipv4_addr() noexcept : ip(0), port(0) {}
    ipv4_addr(uint32_t ip, uint16_t port) noexcept : ip(ip), port(port) {}
    ipv4_addr(uint16_t port) noexcept : ip(0), port(port) {}
    // throws if not a valid ipv4 addr
    ipv4_addr(const std::string &addr);
    ipv4_addr(const std::string &addr, uint16_t port);
    // throws if not an ipv4 addr
    ipv4_addr(const net::inet_address&, uint16_t);
    ipv4_addr(const socket_address &) noexcept;
    ipv4_addr(const ::in_addr&, uint16_t = 0) noexcept;

    bool is_ip_unspecified() const noexcept {
        return ip == 0;
    }
    bool is_port_unspecified() const noexcept {
        return port == 0;
    }
};

struct ipv6_addr {
    using ipv6_bytes = std::array<uint8_t, 16>;

    ipv6_bytes ip;
    uint16_t port;

    ipv6_addr(const ipv6_bytes&, uint16_t port = 0) noexcept;
    ipv6_addr(uint16_t port = 0) noexcept;
    // throws if not a valid ipv6 addr
    ipv6_addr(const std::string&);
    ipv6_addr(const std::string&, uint16_t port);
    ipv6_addr(const net::inet_address&, uint16_t = 0) noexcept;
    ipv6_addr(const ::in6_addr&, uint16_t = 0) noexcept;
    ipv6_addr(const ::sockaddr_in6&) noexcept;
    ipv6_addr(const socket_address&) noexcept;

    bool is_ip_unspecified() const noexcept;
    bool is_port_unspecified() const noexcept {
        return port == 0;
    }
};

std::ostream& operator<<(std::ostream&, const ipv4_addr&);
std::ostream& operator<<(std::ostream&, const ipv6_addr&);

inline bool operator==(const ipv4_addr &lhs, const ipv4_addr& rhs) noexcept {
    return lhs.ip == rhs.ip && lhs.port == rhs.port;
}
SEASTAR_MODULE_EXPORT_END
}

namespace std {
template<>
struct hash<seastar::socket_address> {
    size_t operator()(const seastar::socket_address&) const;
};
template<>
struct hash<seastar::ipv4_addr> {
    size_t operator()(const seastar::ipv4_addr&) const;
};
template<>
struct hash<seastar::unix_domain_addr> {
    size_t operator()(const seastar::unix_domain_addr&) const;
};
template<>
struct hash<::sockaddr_un> {
    size_t operator()(const ::sockaddr_un&) const;
};

template <>
struct hash<seastar::transport> {
    size_t operator()(seastar::transport tr) const {
        return static_cast<size_t>(tr);
    }
};

}

#if FMT_VERSION >= 90000
template <> struct fmt::formatter<seastar::socket_address> : fmt::ostream_formatter {};
template <> struct fmt::formatter<seastar::ipv4_addr> : fmt::ostream_formatter {};
template <> struct fmt::formatter<seastar::ipv6_addr> : fmt::ostream_formatter {};
#endif
