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

#include <seastar/core/seastar.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/with_timeout.hh>
#include <seastar/testing/test_case.hh>
#include <seastar/core/sstring.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/when_all.hh>
#include <seastar/net/api.hh>
#include <seastar/net/dns.hh>
#include <seastar/net/inet_address.hh>

using namespace seastar;
using namespace seastar::net;

static const sstring seastar_name = "seastar.io";

static uint16_t read_be16(const char* p) {
    return (uint16_t(uint8_t(p[0])) << 8) | uint8_t(p[1]);
}

static void write_be16(std::vector<char>& out, uint16_t v) {
    out.push_back(char(v >> 8));
    out.push_back(char(v));
}

static void write_be32(std::vector<char>& out, uint32_t v) {
    out.push_back(char(v >> 24));
    out.push_back(char(v >> 16));
    out.push_back(char(v >> 8));
    out.push_back(char(v));
}

static std::vector<char> make_tcp_dns_a_response(const temporary_buffer<char>& query) {
    BOOST_REQUIRE_GE(query.size(), 12);
    BOOST_REQUIRE_EQUAL(read_be16(query.get() + 4), 1);

    size_t question_end = 12;
    while (question_end < query.size() && query.get()[question_end] != 0) {
        auto label_len = uint8_t(query.get()[question_end]);
        BOOST_REQUIRE_LT(label_len, 64);
        question_end += 1 + label_len;
    }
    BOOST_REQUIRE_LT(question_end, query.size());
    question_end += 1 + sizeof(uint16_t) + sizeof(uint16_t);
    BOOST_REQUIRE_LE(question_end, query.size());

    std::vector<char> msg;
    write_be16(msg, read_be16(query.get()));
    write_be16(msg, 0x8180);
    write_be16(msg, 1);
    write_be16(msg, 1);
    write_be16(msg, 0);
    write_be16(msg, 0);
    msg.insert(msg.end(), query.get() + 12, query.get() + question_end);

    write_be16(msg, 0xc00c);
    write_be16(msg, 1);
    write_be16(msg, 1);
    write_be32(msg, 60);
    write_be16(msg, 4);
    msg.push_back(char(127));
    msg.push_back(char(0));
    msg.push_back(char(0));
    msg.push_back(char(42));

    std::vector<char> tcp_response;
    write_be16(tcp_response, msg.size());
    tcp_response.insert(tcp_response.end(), msg.begin(), msg.end());
    return tcp_response;
}

static future<> serve_split_tcp_dns_response(server_socket& listener) {
    auto ar = co_await listener.accept();
    auto socket = std::move(ar.connection);
    auto in = socket.input();
    auto out = socket.output();

    auto len_buf = co_await in.read_exactly(2);
    BOOST_REQUIRE_EQUAL(len_buf.size(), 2);
    auto query_len = read_be16(len_buf.get());
    auto query = co_await in.read_exactly(query_len);
    BOOST_REQUIRE_EQUAL(query.size(), query_len);

    auto response = make_tcp_dns_a_response(query);
    co_await out.write(response.data(), 3);
    co_await out.flush();
    co_await sleep(std::chrono::milliseconds(10));
    co_await out.write(response.data() + 3, response.size() - 3);
    co_await out.flush();
    co_await out.close();
    co_await in.close();
}

static future<> test_resolve(dns_resolver::options opts) {
    auto d = dns_resolver(std::move(opts));

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    for (auto hostname : {"seastar.io", "scylladb.com", "kernel.org", "www.google.com"}) {
        hostent e = co_await d.get_host_by_name(hostname, inet_address::family::INET);
        BOOST_REQUIRE_EQUAL(e.addr_list.size(), e.addr_entries.size());
        for (auto i = 0ul; i < e.addr_entries.size(); ++i) {
            BOOST_REQUIRE_EQUAL(e.addr_entries[i].addr, e.addr_list[i]);
            BOOST_REQUIRE(e.addr_entries[i].ttl.count() != 0);
        }

        hostent a;
        try {
            a = co_await d.get_host_by_addr(e.addr_entries.front().addr);
        } catch (const std::system_error& e) {
            if (e.code().category() != dns::error_category()) {
                throw;
            }
            continue;
        }
        hostent e2 = co_await d.get_host_by_name(a.names.front(), inet_address::family::INET);
        BOOST_REQUIRE(std::count(e2.addr_list.begin(), e2.addr_list.end(), e.addr_list.front()));
        BOOST_REQUIRE(std::count_if(e2.addr_entries.begin(), e2.addr_entries.end(), [&e](const auto& item){return e.addr_entries.front().addr == item.addr;}));
        BOOST_REQUIRE(!e2.addr_entries.empty());
        BOOST_REQUIRE(e2.addr_entries[0].ttl.count() != 0);
        co_await d.close();
        co_return;
    }
#pragma GCC diagnostic pop
    BOOST_FAIL("No more hosts to try");
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

using enable_if_with_networking = boost::unit_test::enable_if<SEASTAR_TESTING_WITH_NETWORKING>;

SEASTAR_TEST_CASE(test_resolve_numeric,
                  *enable_if_with_networking()) {
    auto d = ::make_lw_shared<dns_resolver>(engine().net(), dns_resolver::options());
    return d->get_host_by_name("127.0.0.1").then_wrapped([d](future<hostent> f) {
        auto ent = f.get();
        BOOST_REQUIRE_EQUAL(ent.addr_entries.size(), 1);
        BOOST_REQUIRE_EQUAL(ent.addr_entries[0].ttl.count(), std::numeric_limits<signed int>::max());
    }).finally([d]{
        return d->close();
    });
}

SEASTAR_TEST_CASE(test_resolve_udp,
                  *enable_if_with_networking()) {
    return test_resolve(dns_resolver::options());
}

SEASTAR_TEST_CASE(test_bad_name_udp,
                  *enable_if_with_networking()) {
    return test_bad_name(dns_resolver::options());
}

SEASTAR_TEST_CASE(test_timeout_udp,
                  *enable_if_with_networking()) {
    dns_resolver::options opts;
    opts.servers = std::vector<inet_address>({ inet_address("1.2.3.4") }); // not a server
    opts.udp_port = 29953; // not a dns port
    opts.timeout = std::chrono::milliseconds(500);

    auto d = ::make_lw_shared<dns_resolver>(engine().net(), opts);
    return d->get_host_by_name(seastar_name, inet_address::family::INET).then_wrapped([d](future<hostent> f) {
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

// NOTE: cannot really test timeout in TCP mode, because seastar sockets do not support
// connect with timeout -> cannot complete connect future in dns::do_connect in reasonable
// time.

// But we can test for connection refused working as expected.
SEASTAR_TEST_CASE(test_connection_refused_tcp) {
    dns_resolver::options opts;
    opts.servers = std::vector<inet_address>({ inet_address("127.0.0.1") });
    opts.use_tcp_query = true;
    opts.tcp_port = 29953; // not a dns port

    auto d = ::make_lw_shared<dns_resolver>(engine().net(), opts);
    return d->get_host_by_name(seastar_name, inet_address::family::INET).then_wrapped([d](future<hostent> f) {
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

SEASTAR_TEST_CASE(test_resolve_tcp_split_response) {
    listen_options lo;
    lo.reuse_address = true;
    lo.set_fixed_cpu(this_shard_id());

    auto listener = seastar::listen(make_ipv4_address({0x7f000001, 0}), lo);
    auto server = serve_split_tcp_dns_response(listener);

    dns_resolver::options opts;
    opts.servers = std::vector<inet_address>({ inet_address("127.0.0.1") });
    opts.use_tcp_query = true;
    opts.tcp_port = listener.local_address().port();
    opts.timeout = std::chrono::seconds(30);

    auto d = ::make_lw_shared<dns_resolver>(engine().net(), opts);

    auto cleanup = [&] () -> future<> {
        co_await d->close();
        listener.abort_accept();
        co_await std::move(server);
    };

    std::exception_ptr ex;
    try {
        auto h = co_await with_timeout(timer<>::clock::now() + std::chrono::seconds(5),
                d->get_host_by_name("split.seastar.test", inet_address::family::INET));
        BOOST_REQUIRE_EQUAL(h.addr_entries.size(), 1);
        BOOST_REQUIRE_EQUAL(h.addr_entries.front().addr, inet_address("127.0.0.42"));
        BOOST_REQUIRE_EQUAL(h.addr_entries.front().ttl, std::chrono::seconds(60));
    } catch (...) {
        ex = std::current_exception();
    }

    try {
        co_await cleanup();
    } catch (...) {
        if (!ex) {
            throw;
        }
    }
    if (ex) {
        std::rethrow_exception(ex);
    }
}

SEASTAR_TEST_CASE(test_resolve_tcp,
                  *enable_if_with_networking()) {
    dns_resolver::options opts;
    opts.use_tcp_query = true;
    return test_resolve(opts);
}

SEASTAR_TEST_CASE(test_bad_name_tcp,
                  *enable_if_with_networking()) {
    dns_resolver::options opts;
    opts.use_tcp_query = true;
    return test_bad_name(opts);
}

static const sstring imaps_service = "imaps";
static const sstring gmail_domain = "gmail.com";

static future<> test_srv() {
    auto d = ::make_lw_shared<dns_resolver>();
    return d->get_srv_records(dns_resolver::srv_proto::tcp,
                              imaps_service,
                              gmail_domain).then([d](dns_resolver::srv_records records) {
        BOOST_REQUIRE(!records.empty());
        for (auto& record : records) {
            // record.target should end with "gmail.com"
            BOOST_REQUIRE_GT(record.target.size(), gmail_domain.size());
            BOOST_REQUIRE_EQUAL(record.target.compare(record.target.size() - gmail_domain.size(),
                                                      gmail_domain.size(),
                                                      gmail_domain),
                                0);
        }
    }).finally([d]{
        return d->close();
    });
}

SEASTAR_TEST_CASE(test_srv_tcp,
                  *enable_if_with_networking()) {
    return test_srv();
}


SEASTAR_TEST_CASE(test_parallel_resolve_name,
                  *enable_if_with_networking()) {
    dns_resolver::options opts;
    opts.use_tcp_query = true;

    auto d = ::make_lw_shared<dns_resolver>(std::move(opts));
    return when_all(
        d->resolve_name("www.google.com"),
        d->resolve_name("www.google.com"),
        d->resolve_name("www.google.com"),
        d->resolve_name("www.google.com"),
        d->resolve_name("www.google.com"),
        d->resolve_name("www.google.com")
    ).finally([d](auto&&...) {}).discard_result();
}

SEASTAR_TEST_CASE(test_parallel_resolve_name_udp,
                  *enable_if_with_networking()) {
    dns_resolver::options opts;

    auto d = ::make_lw_shared<dns_resolver>(std::move(opts));
    return when_all(
        d->resolve_name("www.google.com"),
        d->resolve_name("www.google.com"),
        d->resolve_name("www.google.com"),
        d->resolve_name("www.google.com"),
        d->resolve_name("www.google.com"),
        d->resolve_name("www.google.com")
    ).finally([d](auto&...) {}).discard_result();
}
