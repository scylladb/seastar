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
#include <sys/types.h>
#include <netinet/in.h>
#include <stdexcept>
#include <vector>

#include <seastar/core/future.hh>
#include <seastar/core/sstring.hh>

namespace seastar {
namespace net {

struct ipv4_address;
struct ipv6_address;

class unknown_host : public std::invalid_argument {
public:
    using invalid_argument::invalid_argument;
};

class inet_address {
public:
    enum class family : sa_family_t {
        INET = AF_INET, INET6 = AF_INET6
    };
private:
    family _in_family;

    union {
        ::in_addr _in;
        ::in6_addr _in6;
    };

    uint32_t _scope = invalid_scope;
public:
    static constexpr uint32_t invalid_scope = std::numeric_limits<uint32_t>::max();

    inet_address() noexcept;
    inet_address(family) noexcept;
    inet_address(::in_addr i) noexcept;
    inet_address(::in6_addr i, uint32_t scope = invalid_scope) noexcept;
    // NOTE: does _not_ resolve the address. Only parses
    // ipv4/ipv6 numerical address
    // throws std::invalid_argument if sstring is invalid
    inet_address(const sstring&);
    inet_address(inet_address&&) noexcept = default;
    inet_address(const inet_address&) noexcept = default;

    inet_address(const ipv4_address&) noexcept;
    inet_address(const ipv6_address&, uint32_t scope = invalid_scope) noexcept;

    // throws iff ipv6
    ipv4_address as_ipv4_address() const;
    ipv6_address as_ipv6_address() const noexcept;

    inet_address& operator=(const inet_address&) noexcept = default;
    bool operator==(const inet_address&) const noexcept;

    family in_family() const noexcept {
        return _in_family;
    }

    bool is_ipv6() const noexcept {
        return _in_family == family::INET6;
    }
    bool is_ipv4() const noexcept {
        return _in_family == family::INET;
    }

    size_t size() const noexcept;
    const void * data() const noexcept;

    uint32_t scope() const noexcept {
        return _scope;
    }

    // throws iff ipv6
    operator ::in_addr() const;
    operator ::in6_addr() const noexcept;

    operator ipv6_address() const noexcept;

    future<sstring> hostname() const;
    future<std::vector<sstring>> aliases() const;

    static future<inet_address> find(const sstring&);
    static future<inet_address> find(const sstring&, family);
    static future<std::vector<inet_address>> find_all(const sstring&);
    static future<std::vector<inet_address>> find_all(const sstring&, family);

    static std::optional<inet_address> parse_numerical(const sstring&);

    bool is_loopback() const noexcept;
    bool is_addr_any() const noexcept;
};

std::ostream& operator<<(std::ostream&, const inet_address&);
std::ostream& operator<<(std::ostream&, const inet_address::family&);

}
}

namespace std {
template<>
struct hash<seastar::net::inet_address> {
    size_t operator()(const seastar::net::inet_address&) const;
};
}

#if FMT_VERSION >= 90000
template <> struct fmt::formatter<seastar::net::inet_address> : fmt::ostream_formatter {};
template <> struct fmt::formatter<seastar::net::inet_address::family> : fmt::ostream_formatter {};
#endif
