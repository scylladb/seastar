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
 * Copyright 2016 Cloudius Systems
 */

#pragma once

#include <system_error>
#include <vector>
#include <unordered_map>
#include <memory>
#include <seastar/util/std-compat.hh>

#include <seastar/core/future.hh>
#include <seastar/core/sstring.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/net/inet_address.hh>

namespace seastar {

struct ipv4_addr;

class socket_address;
class network_stack;

/**
 * C-ares based dns query support.
 * Handles name- and ip-based resolution.
 *
 */

namespace net {

/**
 * A c++-esque version of a hostent
 */
struct hostent {
    // Primary name is always first
    std::vector<sstring> names;
    // Primary address is also always first.
    std::vector<inet_address> addr_list;
};

typedef std::optional<inet_address::family> opt_family;

struct srv_record {
    unsigned short priority;
    unsigned short weight;
    unsigned short port;
    sstring target;
};

/**
 * A DNS resolver object.
 * Wraps the query logic & networking.
 * Can be instantiated with options and your network
 * stack of choice, though for "normal" non-test
 * querying, you are probably better of with the
 * global calls further down.
 */
class dns_resolver {
public:
    struct options {
        std::optional<bool>
            use_tcp_query;
        std::optional<std::vector<inet_address>>
            servers;
        std::optional<std::chrono::milliseconds>
            timeout;
        std::optional<uint16_t>
            tcp_port, udp_port;
        std::optional<std::vector<sstring>>
            domains;
    };

    enum class srv_proto {
        tcp, udp
    };
    using srv_records = std::vector<srv_record>;

    dns_resolver();
    dns_resolver(dns_resolver&&) noexcept;
    explicit dns_resolver(const options&);
    explicit dns_resolver(network_stack&, const options& = {});
    ~dns_resolver();

    dns_resolver& operator=(dns_resolver&&) noexcept;

    /**
     * Resolves a hostname to one or more addresses and aliases
     */
    future<hostent> get_host_by_name(const sstring&, opt_family = {});
    /**
     * Resolves an address to one or more addresses and aliases
     */
    future<hostent> get_host_by_addr(const inet_address&);

    /**
     * Resolves a hostname to one (primary) address
     */
    future<inet_address> resolve_name(const sstring&, opt_family = {});
    /**
     * Resolves an address to one (primary) name
     */
    future<sstring> resolve_addr(const inet_address&);

    /**
     * Resolve a service in given domain to one or more SRV records
     */
    future<srv_records> get_srv_records(srv_proto proto,
                                        const sstring& service,
                                        const sstring& domain);

    /**
     * Shuts the object down. Great for tests.
     */
    future<> close();
private:
    class impl;
    shared_ptr<impl> _impl;
};

namespace dns {

// See above. These functions simply queries using a shard-local
// default-stack, default-opts resolver
future<hostent> get_host_by_name(const sstring&, opt_family = {});
future<hostent> get_host_by_addr(const inet_address&);

future<inet_address> resolve_name(const sstring&, opt_family = {});
future<sstring> resolve_addr(const inet_address&);

future<std::vector<srv_record>> get_srv_records(dns_resolver::srv_proto proto,
                                                const sstring& service,
                                                const sstring& domain);

/**
 * Error handling.
 *
 * The error_category instance used by exceptions thrown by DNS
 */
 const std::error_category& error_category();

}

}

}
