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
#include <sys/socket.h>
#include <netinet/ip.h>
#include "net/byteorder.hh"

namespace seastar {

struct ipv4_addr;

class socket_address {
public:
    union {
        ::sockaddr_storage sas;
        ::sockaddr sa;
        ::sockaddr_in in;
    } u;
    socket_address(sockaddr_in sa) {
        u.in = sa;
    }
    socket_address(ipv4_addr);
    socket_address() = default;
    ::sockaddr& as_posix_sockaddr() { return u.sa; }
    ::sockaddr_in& as_posix_sockaddr_in() { return u.in; }
    const ::sockaddr& as_posix_sockaddr() const { return u.sa; }
    const ::sockaddr_in& as_posix_sockaddr_in() const { return u.in; }

    bool operator==(const socket_address&) const;
};

std::ostream& operator<<(std::ostream&, const socket_address&);

enum class transport {
    TCP = IPPROTO_TCP,
    SCTP = IPPROTO_SCTP
};


namespace net {
class inet_address;
}

struct ipv4_addr {
    uint32_t ip;
    uint16_t port;

    ipv4_addr() : ip(0), port(0) {}
    ipv4_addr(uint32_t ip, uint16_t port) : ip(ip), port(port) {}
    ipv4_addr(uint16_t port) : ip(0), port(port) {}
    ipv4_addr(const std::string &addr);
    ipv4_addr(const std::string &addr, uint16_t port);
    ipv4_addr(const net::inet_address&, uint16_t);

    ipv4_addr(const socket_address &sa) {
        ip = net::ntoh(sa.u.in.sin_addr.s_addr);
        port = net::ntoh(sa.u.in.sin_port);
    }

    ipv4_addr(socket_address &&sa) : ipv4_addr(sa) {}
};

}
