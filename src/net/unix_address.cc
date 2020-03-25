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
 * Copyright (C) 2019 Red Hat, Inc.
 */
/*! \file
  \brief unix-domain address structures, to be used for creating socket_address-es for unix-domain
         sockets.

  Note that the path in a unix-domain address may start with a null character.
*/

#include <ostream>
#include <seastar/net/socket_defs.hh>
#include <cassert>

namespace seastar {

std::ostream& operator<<(std::ostream& os, const unix_domain_addr& addr) {
    if (addr.path_length() == 0) {
        return os << "{unnamed}";
    }
    if (addr.name[0]) {
        // regular (filesystem-namespace) path
        return os << addr.name;
    }

    os << '@';
    const char* src = addr.path_bytes() + 1;

    for (auto k = addr.path_length(); --k > 0; src++) {
        os << (std::isprint(*src) ? *src : '_');
    }
    return os;
}

} // namespace seastar

size_t std::hash<seastar::unix_domain_addr>::operator()(const seastar::unix_domain_addr& a) const {
    return std::hash<std::string>()(a.name);
}
