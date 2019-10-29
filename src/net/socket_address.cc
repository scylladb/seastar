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
/*! \file
    \brief Some non-INET-specific socket address code

    Extracted from inet_address.cc.
 */
#include <ostream>
#include <arpa/inet.h>
#include <seastar/net/socket_defs.hh>
#include <seastar/net/inet_address.hh>
#include <seastar/net/ip.hh>
#include <seastar/core/print.hh>
#include <boost/functional/hash.hpp>


using namespace std::string_literals;

size_t std::hash<seastar::socket_address>::operator()(const seastar::socket_address& a) const {
    auto h = std::hash<seastar::net::inet_address>()(a.addr());
    boost::hash_combine(h, a.as_posix_sockaddr_in().sin_port);
    return h;
}

namespace seastar {

socket_address::socket_address(const net::inet_address& a, uint16_t p)
    : socket_address(a.is_ipv6() ? socket_address(ipv6_addr(a, p), a.scope()) : socket_address(ipv4_addr(a, p)))
{}

socket_address::socket_address(const unix_domain_addr& s) {
    u.un.sun_family = AF_UNIX;
    memset(u.un.sun_path, '\0', sizeof(u.un.sun_path));
    auto path_length = std::min((int)sizeof(u.un.sun_path), s.path_length());
    memcpy(u.un.sun_path, s.path_bytes(), path_length);
    addr_length = path_length + offsetof(struct ::sockaddr_un, sun_path);
}

std::string unix_domain_addr_text(const socket_address& sa) {
    if (sa.length() <= ((size_t) (((struct ::sockaddr_un *) 0)->sun_path))) {
        return "{unnamed}"s;
    }
    if (sa.u.un.sun_path[0]) {
        // regular (filesystem-namespace) path
        return std::string{sa.u.un.sun_path};
    }

    const size_t  path_length{sa.length() - ((size_t) (((struct ::sockaddr_un *) 0)->sun_path))};
    char ud_path[1 + path_length];
    char* targ = ud_path;
    *targ++ = '@';
    const char* src = sa.u.un.sun_path + 1;
    int k = (int)path_length;

    for (; --k > 0; src++) {
        *targ++ = std::isprint(*src) ? *src : '_';
    }
    return std::string{ud_path, path_length};
}

std::ostream& operator<<(std::ostream& os, const socket_address& a) {
    if (a.is_af_unix()) {
        return os << unix_domain_addr_text(a);
    }

    auto addr = a.addr();
    // CMH. maybe skip brackets for ipv4-mapped
    auto bracket = addr.in_family() == seastar::net::inet_address::family::INET6;

    if (bracket) {
        os << '[';
    }
    os << addr;
    if (bracket) {
        os << ']';
    }

    return os << ':' << ntohs(a.u.in.sin_port);
}

} // namespace seastar
