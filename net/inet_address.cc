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

#include <ostream>
#include <arpa/inet.h>

#include "inet_address.hh"
#include "socket_defs.hh"
#include "dns.hh"

namespace seastar {

seastar::net::inet_address::inet_address()
                : inet_address(::in_addr{ 0, })
{}

seastar::net::inet_address::inet_address(::in_addr i)
                : _in_family(family::INET), _in(i) {
}

seastar::net::inet_address::inet_address(::in6_addr i)
                : _in_family(family::INET6), _in6(i) {
}

seastar::net::inet_address::inet_address(const sstring& addr)
                : inet_address([&addr] {
    inet_address in;
    if (::inet_pton(AF_INET, addr.c_str(), &in._in)) {
        in._in_family = family::INET;
        return in;
    }
    if (::inet_pton(AF_INET6, addr.c_str(), &in._in6)) {
        in._in_family = family::INET6;
        return in;
    }
    throw std::invalid_argument(addr);
}())
{}

bool seastar::net::inet_address::operator==(const inet_address& o) const {
    if (o._in_family != _in_family) {
        return false;
    }
    switch (_in_family) {
    case family::INET:
        return _in.s_addr == o._in.s_addr;
    case family::INET6:
        return std::equal(std::begin(_in6.s6_addr), std::end(_in6.s6_addr),
                        std::begin(o._in6.s6_addr));
    default:
        return false;
    }
}

seastar::net::inet_address::operator const ::in_addr&() const {
    if (_in_family != family::INET) {
        throw std::invalid_argument("Not an ipv4 address");
    }
    return _in;
}

seastar::net::inet_address::operator const ::in6_addr&() const {
    if (_in_family != family::INET6) {
        throw std::invalid_argument("Not an ipv6 address");
    }
    return _in6;
}

size_t seastar::net::inet_address::size() const {
    switch (_in_family) {
    case family::INET:
        return sizeof(::in_addr);
    case family::INET6:
        return sizeof(::in6_addr);
    default:
        return 0;
    }
}

const void * seastar::net::inet_address::data() const {
    return &_in;
}


std::ostream& seastar::net::operator<<(std::ostream& os, const inet_address& addr) {
    char buffer[64];
    return os << inet_ntop(int(addr.in_family()), addr.data(), buffer, sizeof(buffer));
}

std::ostream& seastar::net::operator<<(std::ostream& os, const inet_address::family& f) {
    switch (f) {
    case inet_address::family::INET:
        os << "INET";
        break;
    case inet_address::family::INET6:
        os << "INET6";
        break;
    default:
        break;
    }
    return os;
}

std::ostream& operator<<(std::ostream& os, const socket_address& a) {
    return os << seastar::net::inet_address(a.as_posix_sockaddr_in().sin_addr)
        << ":" << a.u.in.sin_port
        ;
}

}

