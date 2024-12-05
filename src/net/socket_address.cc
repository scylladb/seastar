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
#ifdef SEASTAR_MODULE
module;
#endif

#include <arpa/inet.h>
#include <sys/un.h>
#include <ostream>
#include <boost/functional/hash.hpp>

#ifdef SEASTAR_MODULE
module seastar;
#else
#include <seastar/net/socket_defs.hh>
#include <seastar/net/inet_address.hh>
#include <seastar/net/ip.hh>
#include <seastar/core/print.hh>
#endif

using namespace std::string_literals;

size_t std::hash<seastar::socket_address>::operator()(const seastar::socket_address& a) const {
    auto h = std::hash<seastar::net::inet_address>()(a.addr());
    boost::hash_combine(h, a.as_posix_sockaddr_in().sin_port);
    return h;
}

namespace seastar {

static_assert(std::is_nothrow_default_constructible_v<socket_address>);
static_assert(std::is_nothrow_copy_constructible_v<socket_address>);
static_assert(std::is_nothrow_move_constructible_v<socket_address>);

socket_address::socket_address() noexcept
    // set max addr_length, as we (probably) want to use the constructed object
    // in accept() or get_address()
    : addr_length(sizeof(::sockaddr_storage))
{
    static_assert(AF_UNSPEC == 0, "just checking");
    memset(&u, 0, sizeof(u));
}

socket_address::socket_address(uint16_t p) noexcept
    : socket_address(ipv4_addr(p))
{}

socket_address::socket_address(ipv4_addr addr) noexcept
{
    addr_length = sizeof(::sockaddr_in);
    u.in.sin_family = AF_INET;
    u.in.sin_port = htons(addr.port);
    u.in.sin_addr.s_addr = htonl(addr.ip);
}

socket_address::socket_address(const ipv6_addr& addr, uint32_t scope) noexcept
{
    addr_length = sizeof(::sockaddr_in6);
    u.in6.sin6_family = AF_INET6;
    u.in6.sin6_port = htons(addr.port);
    u.in6.sin6_flowinfo = 0;
    u.in6.sin6_scope_id = scope;
    std::copy(addr.ip.begin(), addr.ip.end(), u.in6.sin6_addr.s6_addr);
}

socket_address::socket_address(const ipv6_addr& addr) noexcept
    : socket_address(addr, net::inet_address::invalid_scope)
{}

socket_address::socket_address(uint32_t ipv4, uint16_t p) noexcept
    : socket_address(make_ipv4_address(ipv4, p))
{}

socket_address::socket_address(const net::inet_address& a, uint16_t p) noexcept
    : socket_address(a.is_ipv6() ? socket_address(ipv6_addr(a, p), a.scope()) : socket_address(ipv4_addr(a, p)))
{}

socket_address::socket_address(const unix_domain_addr& s) noexcept {
    u.un.sun_family = AF_UNIX;
    memset(u.un.sun_path, '\0', sizeof(u.un.sun_path));
    auto path_length = std::min((int)sizeof(u.un.sun_path), s.path_length());
    memcpy(u.un.sun_path, s.path_bytes(), path_length);
    addr_length = path_length + offsetof(struct ::sockaddr_un, sun_path);
}

bool socket_address::is_unspecified() const noexcept {
    return u.sa.sa_family == AF_UNSPEC;
}

static int adjusted_path_length(const socket_address& a) noexcept {
    int l = std::max(0, (int)a.addr_length-(int)(offsetof(sockaddr_un, sun_path)));
    // "un-count" a trailing null in filesystem-namespace paths
    if (a.u.un.sun_path[0]!='\0' && (l > 1) && a.u.un.sun_path[l-1]=='\0') {
        --l;
    }
    return l;
}

bool socket_address::operator==(const socket_address& a) const noexcept {
    if (u.sa.sa_family != a.u.sa.sa_family) {
        return false;
    }
    if (u.sa.sa_family == AF_UNIX) {
        // tolerate counting/not counting a terminating null in filesystem-namespace paths
        int adjusted_len = adjusted_path_length(*this);
        int a_adjusted_len = adjusted_path_length(a);
        if (adjusted_len != a_adjusted_len) {
            return false;
        }
        return (memcmp(u.un.sun_path, a.u.un.sun_path, adjusted_len) == 0);
    }

    // an INET address
    if (u.in.sin_port != a.u.in.sin_port) {
        return false;
    }
    switch (u.sa.sa_family) {
    case AF_INET:
        return u.in.sin_addr.s_addr == a.u.in.sin_addr.s_addr;
    case AF_UNSPEC:
    case AF_INET6:
        // handled below
        break;
    default:
        return false;
    }

    auto& in1 = as_posix_sockaddr_in6();
    auto& in2 = a.as_posix_sockaddr_in6();

    return IN6_ARE_ADDR_EQUAL(&in1.sin6_addr, &in2.sin6_addr);
}

std::string unix_domain_addr_text(const socket_address& sa) {
    if (sa.length() <= offsetof(sockaddr_un, sun_path)) {
        return "{unnamed}"s;
    }
    if (sa.u.un.sun_path[0]) {
        // regular (filesystem-namespace) path
        return std::string{sa.u.un.sun_path};
    }

    const size_t  path_length{sa.length() - offsetof(sockaddr_un, sun_path)};
    std::string ud_path(path_length, 0);
    char* targ = &ud_path[0];
    *targ++ = '@';
    const char* src = sa.u.un.sun_path + 1;
    int k = (int)path_length;

    for (; --k > 0; src++) {
        *targ++ = std::isprint(*src) ? *src : '_';
    }
    return ud_path;
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
