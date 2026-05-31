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
 * Copyright 2024 ScyllaDB
 */

#include "loopback_socket.hh"
#include "test_comparisons.hh"

#include <seastar/core/metrics.hh>
#include <seastar/core/prometheus.hh>
#include <seastar/http/client.hh>
#include <seastar/http/common.hh>
#include <seastar/http/request.hh>
#include <seastar/testing/test_case.hh>
#include <seastar/testing/thread_test_case.hh>
#include <seastar/util/closeable.hh>
#include <seastar/util/short_streams.hh>

#include <boost/test/tools/old/interface.hpp>

using namespace seastar;
using namespace httpd;
using namespace std::literals;

namespace {

class loopback_http_factory : public http::connection_factory {
    loopback_socket_impl lsi;
public:
    explicit loopback_http_factory(loopback_connection_factory& f) : lsi(f) {}
    virtual future<connected_socket> make(abort_source*) override {
        return lsi.connect(socket_address(ipv4_addr()), socket_address(ipv4_addr()));
    }
};

// Issue a GET to `path` and return the full response body as a string.
std::string get_metrics_body(loopback_connection_factory& lcf, const sstring& path) {
    auto cln = http::client(std::make_unique<loopback_http_factory>(lcf));
    std::string body;
    cln.make_request(http::request::make("GET", "test", path),
        [&body] (const http::reply& rep, input_stream<char>&& in) {
            SEASTAR_BOOST_REQUIRE_EQUAL(rep._status, http::reply::status_type::ok);
            return seastar::async([&body, in = std::move(in)] () mutable {
                body = util::read_entire_stream_contiguous(in).get();
                in.close().get();
            });
        }, http::reply::status_type::ok).get();
    cln.close().get();
    return body;
}

struct test_metrics {
    metrics::metric_groups _metrics;

    void setup_metrics() {
        auto somelabel = metrics::label("somekey");

        _metrics.add_group("aaaa", {
            metrics::make_gauge("escaped_label_value_test", [] { return 10; }, metrics::description{"test that special characters are escaped"}, {somelabel(R"(special"\nvalue)")}),
            metrics::make_gauge("int_test", [] { return 10; }, metrics::description{"simple minimal test"}),
            metrics::make_gauge("double_test", [] { return 1234567654321.0; }, metrics::description{"test that a long double is printed fully and not in scientific notation"}),
            metrics::make_counter("counter_test", [] () -> int64_t { return 1234567654321; }, metrics::description{"test with a long counter value"}),
        });
    }
};

struct test_case {
    // iff true, add a global label in prometheus config
    bool use_global_label;
};

static const metrics::label_instance global_label{"global", "foo"};

future<> test_prometheus_metrics_body(test_case tc) {

    test_metrics metrics;
    metrics.setup_metrics();

    co_await seastar::async([tc] {
        loopback_connection_factory lcf(1);
        http_server server("test");
        httpd::http_server_tester::listeners(server).emplace_back(lcf.get_server_socket());

        prometheus::config ctx;
        if (tc.use_global_label) {
            ctx.label = global_label;
        }
        add_prometheus_routes(server, ctx).get();

        future<> client = seastar::async([&lcf, tc] {
            auto resp_str = get_metrics_body(lcf, "/metrics");

            auto global_label_str = tc.use_global_label ? fmt::format("{}=\"{}\",", global_label.key(), global_label.value()) : std::string{};

            std::string expected0 = R"(seastar_aaaa_escaped_label_value_test{)" + global_label_str + R"(shard="0",somekey="special\"\\nvalue"} 10.000000)";
            std::string expected1 = R"(seastar_aaaa_int_test{)" + global_label_str + R"(shard="0"} 10.000000)";
            std::string expected2 = R"(seastar_aaaa_double_test{)" + global_label_str + R"(shard="0"} 1234567654321.000000)";
            std::string expected3 = R"(seastar_aaaa_counter_test{)" + global_label_str + R"(shard="0"} 1234567654321)";

            auto all_expected = {expected0, expected1, expected2, expected3};

            for (auto& expected : all_expected) {
                BOOST_REQUIRE_MESSAGE(std::ranges::search(resp_str, expected),
                    fmt::format("cannot find: {}\nResponse: {}\n", expected, resp_str));
            }
        });

        server.do_accepts(0).get();

        client.get();
        server.stop().get();
    });
}

}

SEASTAR_TEST_CASE(test_prometheus_metrics) {
    return test_prometheus_metrics_body({.use_global_label = false});
}

SEASTAR_TEST_CASE(test_prometheus_metrics_global_label) {
    return test_prometheus_metrics_body({.use_global_label = true});
}

// Test that multiple __name__ query parameters filter to only matching metrics
SEASTAR_TEST_CASE(test_prometheus_multiple_name_filters) {
    // Create metrics with distinct names
    metrics::metric_groups test_metrics;
    test_metrics.add_group("test", {
        metrics::make_gauge("metric_alpha", [] { return 1; }, metrics::description{"alpha metric"}),
        metrics::make_gauge("metric_beta", [] { return 2; }, metrics::description{"beta metric"}),
        metrics::make_gauge("metric_gamma", [] { return 3; }, metrics::description{"gamma metric"}),
    });

    co_await seastar::async([] {
        loopback_connection_factory lcf(1);
        http_server server("test");
        httpd::http_server_tester::listeners(server).emplace_back(lcf.get_server_socket());

        prometheus::config ctx;
        add_prometheus_routes(server, ctx).get();

        future<> client = seastar::async([&lcf] {
            // Request only metric_alpha and metric_gamma using multiple __name__ parameters
            auto resp_str = get_metrics_body(lcf, "/metrics?__name__=test_metric_alpha&__name__=test_metric_gamma");

            // Should contain alpha and gamma
            BOOST_REQUIRE_MESSAGE(std::ranges::search(resp_str, "seastar_test_metric_alpha"sv),
                fmt::format("should contain metric_alpha\nResponse: {}\n", resp_str));
            BOOST_REQUIRE_MESSAGE(std::ranges::search(resp_str, "seastar_test_metric_gamma"sv),
                fmt::format("should contain metric_gamma\nResponse: {}\n", resp_str));

            // Should NOT contain beta
            BOOST_REQUIRE_MESSAGE(!std::ranges::search(resp_str, "seastar_test_metric_beta"sv),
                fmt::format("should NOT contain metric_beta\nResponse: {}\n", resp_str));
        });

        server.do_accepts(0).get();

        client.get();
        server.stop().get();
    });
}
