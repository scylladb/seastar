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
#include <vector>
#include <algorithm>

#include "core/do_with.hh"
#include "test-utils.hh"
#include "core/sstring.hh"
#include "core/reactor.hh"
#include "core/do_with.hh"
#include "core/future-util.hh"
#include "net/dns.hh"
#include "net/inet_address.hh"

using namespace seastar;
using namespace seastar::net;

static const inet_address google_addr = inet_address("216.58.201.164");
static const sstring google_name = "www.google.com";

static future<> test_resolve(dns_resolver::options opts) {
    auto d = ::make_lw_shared<dns_resolver>(std::move(opts));
    return d->get_host_by_name(google_name, inet_address::family::INET).then([d](hostent e) {
        //BOOST_REQUIRE(std::count(e.addr_list.begin(), e.addr_list.end(), google_addr));
        return d->get_host_by_addr(e.addr_list.front()).then([d, a = e.addr_list.front()](hostent e) {
            return d->get_host_by_name(e.names.front(), inet_address::family::INET).then([a](hostent e) {
                BOOST_REQUIRE(std::count(e.addr_list.begin(), e.addr_list.end(), a));
            });
        });
    }).finally([d]{
        return d->close();
    });
}

static future<> test_bad_name(dns_resolver::options opts) {
    auto d = ::make_lw_shared<dns_resolver>(std::move(opts));
    return d->get_host_by_name("apa.ninja.gnu", inet_address::family::INET).then_wrapped([d](future<hostent> f) {
        try {
            f.get();
            BOOST_FAIL("should not succeed");
        } catch (...) {
            // ok.
        }
    }).finally([d]{
        return d->close();
    });
}

SEASTAR_TEST_CASE(test_resolve_udp) {
    return test_resolve(dns_resolver::options());
}

SEASTAR_TEST_CASE(test_bad_name_udp) {
    return test_bad_name(dns_resolver::options());
}

SEASTAR_TEST_CASE(test_timeout_udp) {
    dns_resolver::options opts;
    opts.servers = std::vector<inet_address>({ inet_address("1.2.3.4") }); // not a server
    opts.timeout = std::chrono::milliseconds(500);

    auto d = ::make_lw_shared<dns_resolver>(engine().net(), opts);
    return d->get_host_by_name(google_name, inet_address::family::INET).then_wrapped([d](future<hostent> f) {
        try {
            f.get();
            BOOST_FAIL("should not succeed");
        } catch (...) {
            // ok.
        }
    }).finally([d]{
        return d->close();
    });
}

SEASTAR_TEST_CASE(test_resolve_tcp) {
    dns_resolver::options opts;
    opts.use_tcp_query = true;
    return test_resolve(opts);
}

SEASTAR_TEST_CASE(test_bad_name_tcp) {
    dns_resolver::options opts;
    opts.use_tcp_query = true;
    return test_bad_name(opts);
}

