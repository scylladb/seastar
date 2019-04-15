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

#include <iosfwd>
#include <array>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <seastar/net/byteorder.hh>

namespace seastar {

namespace net {
class inet_address;
}

struct ipv4_addr;
struct ipv6_addr;

class socket_address {
public:
    union {
        ::sockaddr_storage sas;
        ::sockaddr sa;
        ::sockaddr_in in;
        ::sockaddr_in6 in6;
    } u;
    socket_address(const sockaddr_in& sa) {
        u.in = sa;
    }
    socket_address(const sockaddr_in6& sa) {
        u.in6 = sa;
    }
    socket_address(uint16_t);
    socket_address(ipv4_addr);
    socket_address(const ipv6_addr&);
    socket_address(const net::inet_address&, uint16_t p = 0);
    socket_address();
    ::sockaddr& as_posix_sockaddr() { return u.sa; }
    ::sockaddr_in& as_posix_sockaddr_in() { return u.in; }
    ::sockaddr_in6& as_posix_sockaddr_in6() { return u.in6; }
    const ::sockaddr& as_posix_sockaddr() const { return u.sa; }
    const ::sockaddr_in& as_posix_sockaddr_in() const { return u.in; }
    const ::sockaddr_in6& as_posix_sockaddr_in6() const { return u.in6; }

    socket_address(uint32_t, uint16_t p = 0);

    net::inet_address addr() const;
    ::in_port_t port() const;
    bool is_wildcard() const;

    bool operator==(const socket_address&) const;
    bool operator!=(const socket_address& a) const {
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

    ipv4_addr() : ip(0), port(0) {}
    ipv4_addr(uint32_t ip, uint16_t port) : ip(ip), port(port) {}
    ipv4_addr(uint16_t port) : ip(0), port(port) {}
    ipv4_addr(const std::string &addr);
    ipv4_addr(const std::string &addr, uint16_t port);
    ipv4_addr(const net::inet_address&, uint16_t);
    ipv4_addr(const socket_address &);
    ipv4_addr(const ::in_addr&, uint16_t = 0);

    bool is_ip_unspecified() const {
        return ip == 0;
    }
    bool is_port_unspecified() const {
        return port == 0;
    }
};

struct ipv6_addr {
    using ipv6_bytes = std::array<uint8_t, 16>;

    ipv6_bytes ip;
    uint16_t port;

    ipv6_addr(const ipv6_bytes&, uint16_t port = 0);
    ipv6_addr(uint16_t port = 0);
    ipv6_addr(const std::string&);
    ipv6_addr(const std::string&, uint16_t port);
    ipv6_addr(const net::inet_address&, uint16_t = 0);
    ipv6_addr(const ::in6_addr&, uint16_t = 0);
    ipv6_addr(const ::sockaddr_in6&);
    ipv6_addr(const socket_address&);

    bool is_ip_unspecified() const;
    bool is_port_unspecified() const {
        return port == 0;
    }
};

std::ostream& operator<<(std::ostream&, const ipv4_addr&);
std::ostream& operator<<(std::ostream&, const ipv6_addr&);

inline bool operator==(const ipv4_addr &lhs, const ipv4_addr& rhs) {
    return lhs.ip == rhs.ip && lhs.port == rhs.port;
}

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

}
