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

#include <seastar/core/metrics.hh>
#include <seastar/core/prometheus.hh>
#include <seastar/http/client.hh>
#include <seastar/http/common.hh>
#include <seastar/http/request.hh>
#include <seastar/testing/test_case.hh>
#include <seastar/testing/thread_test_case.hh>
#include <seastar/util/closeable.hh>
#include <seastar/util/short_streams.hh>

#include <seastar/core/reactor.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/thread.hh>
#include <seastar/util/later.hh>

#include <boost/test/tools/old/interface.hpp>

#include <chrono>
#include <cstdlib>
#include <functional>
#include <vector>

#ifdef SEASTAR_HAVE_ZLIB
#include <zlib.h>
#endif
#ifdef SEASTAR_HAVE_ZSTD
#include <zstd.h>
#endif

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

const sstring accept_protobuf = "application/vnd.google.protobuf; proto=io.prometheus.client.MetricFamily; encoding=delimited";

struct metrics_reply {
    std::string body; // as sent, that is compressed if content_encoding is set
    sstring content_encoding;
    sstring vary;
};

// Issue a GET to `path` and return the response body, as it was sent, and
// the headers that describe its coding.
// `on_received` is called once the whole reply was received, before the body,
// which arrives in fragments, is copied to the string that is returned.
metrics_reply get_metrics(loopback_connection_factory& lcf, const sstring& path, const sstring& accept_encoding = "", const sstring& accept = "",
        std::function<void ()> on_received = {}) {
    auto cln = http::client(std::make_unique<loopback_http_factory>(lcf));
    metrics_reply ret;
    // Kept as received and joined only once the request is over: the callers
    // that watch for reactor stalls must not get one from copying a large body.
    std::vector<temporary_buffer<char>> fragments;
    auto req = http::request::make("GET", "test", path);
    if (!accept_encoding.empty()) {
        req._headers["Accept-Encoding"] = accept_encoding;
    }
    if (!accept.empty()) {
        req._headers["Accept"] = accept;
    }
    cln.make_request(std::move(req),
        [&ret, &fragments] (const http::reply& rep, input_stream<char>&& in) {
            BOOST_REQUIRE_EQUAL(rep._status, http::reply::status_type::ok);
            ret.content_encoding = rep.get_header("Content-Encoding");
            ret.vary = rep.get_header("Vary");
            return seastar::async([&fragments, in = std::move(in)] () mutable {
                while (auto buf = in.read().get()) {
                    fragments.push_back(std::move(buf));
                }
                in.close().get();
            });
        }, http::reply::status_type::ok).get();
    cln.close().get();
    if (on_received) {
        on_received();
    }
    for (auto& buf : fragments) {
        ret.body.append(buf.get(), buf.size());
    }
    return ret;
}

// Issue a GET to `path` and return the full response body as a string.
std::string get_metrics_body(loopback_connection_factory& lcf, const sstring& path) {
    return get_metrics(lcf, path).body;
}

#ifdef SEASTAR_HAVE_ZLIB
std::string gunzip(std::string_view in) {
    z_stream zs{};
    BOOST_REQUIRE_EQUAL(inflateInit2(&zs, 15 + 16), Z_OK);
    std::string out;
    char buf[64 * 1024];
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
    zs.avail_in = in.size();
    int r;
    do {
        zs.next_out = reinterpret_cast<Bytef*>(buf);
        zs.avail_out = sizeof(buf);
        r = inflate(&zs, Z_NO_FLUSH);
        BOOST_REQUIRE_MESSAGE(r == Z_OK || r == Z_STREAM_END, fmt::format("inflate failed: {}", r));
        out.append(buf, sizeof(buf) - zs.avail_out);
    } while (r != Z_STREAM_END);
    // The body must be exactly one complete gzip stream
    BOOST_REQUIRE_EQUAL(zs.avail_in, 0u);
    inflateEnd(&zs);
    return out;
}
#endif

#ifdef SEASTAR_HAVE_ZSTD
std::string unzstd(std::string_view in) {
    auto* ctx = ZSTD_createDCtx();
    std::string out;
    char buf[64 * 1024];
    ZSTD_inBuffer ib{in.data(), in.size(), 0};
    size_t r;
    do {
        ZSTD_outBuffer ob{buf, sizeof(buf), 0};
        r = ZSTD_decompressStream(ctx, &ob, &ib);
        BOOST_REQUIRE_MESSAGE(!ZSTD_isError(r), fmt::format("zstd decompression failed: {}", ZSTD_getErrorName(r)));
        out.append(buf, ob.pos);
        // No progress with a frame still open means that the body is truncated
        BOOST_REQUIRE(r == 0 || ob.pos > 0 || ib.pos < ib.size);
    } while (r != 0);
    // The body must be exactly one complete frame
    BOOST_REQUIRE_EQUAL(ib.pos, ib.size);
    ZSTD_freeDCtx(ctx);
    return out;
}
#endif

// A content coding that this build of Seastar supports, and how to undo it
struct test_coding {
    prometheus::compression_type type;
    sstring name;
    std::string (*decompress)(std::string_view);
};

std::vector<test_coding> supported_codings() {
    return {
#ifdef SEASTAR_HAVE_ZLIB
        {prometheus::compression_type::gzip, "gzip", gunzip},
#endif
#ifdef SEASTAR_HAVE_ZSTD
        {prometheus::compression_type::zstd, "zstd", unzstd},
#endif
    };
}

// Registers `series` gauges in families of `family_size`, under the group "comp"
metrics::metric_groups make_series(size_t series, size_t family_size) {
    metrics::metric_groups groups;
    auto instance = metrics::label("instance");
    std::vector<metrics::metric_definition> defs;
    for (size_t i = 0; i < series; i++) {
        defs.push_back(metrics::make_gauge(fmt::format("family_{}", i / family_size), [i] { return i; },
                metrics::description{"a metric to compress"}, {instance(fmt::format("series-{}", i))}));
    }
    groups.add_group("comp", defs);
    return groups;
}

// Serves the metrics with `ctx` for the duration of `client`, which runs in a thread
future<> with_prometheus_server(prometheus::config ctx, std::function<void (loopback_connection_factory&)> client) {
    return seastar::async([ctx = std::move(ctx), client = std::move(client)] {
        loopback_connection_factory lcf(1);
        http_server server("test");
        server.listen(lcf.get_server_socket()).get();
        auto stop_server = defer([&server] () noexcept { server.stop().get(); });
        add_prometheus_routes(server, ctx).get();
        client(lcf);
    });
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
        server.listen(lcf.get_server_socket()).get();

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
        server.listen(lcf.get_server_socket()).get();

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

        client.get();
        server.stop().get();
    });
}

SEASTAR_TEST_CASE(test_prometheus_parse_compression) {
    using enum prometheus::compression_type;
    using types = std::vector<prometheus::compression_type>;
    BOOST_REQUIRE(prometheus::parse_compression("") == types{});
    BOOST_REQUIRE(prometheus::parse_compression("none") == types{});
    BOOST_REQUIRE(prometheus::parse_compression("gzip") == types{gzip});
    BOOST_REQUIRE(prometheus::parse_compression("zstd") == types{zstd});
    BOOST_REQUIRE((prometheus::parse_compression(" GZip , zstd") == types{gzip, zstd}));
    BOOST_REQUIRE_THROW(prometheus::parse_compression("gzip,lzma"), std::invalid_argument);
    return make_ready_future<>();
}

SEASTAR_TEST_CASE(test_prometheus_select_compression) {
    using enum prometheus::compression_type;
    using prometheus::details::select_compression;
    using types = std::vector<prometheus::compression_type>;
    const types both{gzip, zstd};

    // Nothing enabled, or nothing accepted
    BOOST_REQUIRE(!select_compression("gzip, zstd", {}));
    BOOST_REQUIRE(!select_compression("", both));
    BOOST_REQUIRE(!select_compression("identity", both));
    BOOST_REQUIRE(!select_compression("*;q=0", both));
    BOOST_REQUIRE(!select_compression("br, deflate", both));
    // A coding is used only if it is both enabled and accepted
    BOOST_REQUIRE(select_compression("gzip", both) == gzip);
    BOOST_REQUIRE(select_compression("zstd", both) == zstd);
    BOOST_REQUIRE(!select_compression("zstd", types{gzip}));
    BOOST_REQUIRE(select_compression("zstd,gzip", types{gzip}) == gzip);
    BOOST_REQUIRE(select_compression("deflate, GZIP ;q=0.5", types{gzip}) == gzip);
    // zstd is preferred, whatever the order of either list
    BOOST_REQUIRE(select_compression("gzip, zstd", both) == zstd);
    BOOST_REQUIRE(select_compression("zstd;q=0.1, gzip;q=1.0", types{zstd, gzip}) == zstd);
    // A zero weight refuses the coding
    BOOST_REQUIRE(select_compression("zstd;q=0, gzip", both) == gzip);
    BOOST_REQUIRE(!select_compression("gzip; q=0.0", both));
    // "*" stands for the codings that the header does not name
    BOOST_REQUIRE(select_compression("*", both) == zstd);
    BOOST_REQUIRE(select_compression("*", types{gzip}) == gzip);
    BOOST_REQUIRE(select_compression("*;q=1, identity;q=0", types{gzip}) == gzip);
    BOOST_REQUIRE(select_compression("zstd;q=0, *", both) == gzip);
    BOOST_REQUIRE(!select_compression("gzip;q=0, zstd;q=0, *", both));
    return make_ready_future<>();
}

// Compression is off unless it is configured, whatever the client accepts
SEASTAR_TEST_CASE(test_prometheus_compression_off_by_default) {
    auto metrics = make_series(10, 10);
    co_await with_prometheus_server(prometheus::config{}, [] (loopback_connection_factory& lcf) {
        auto reply = get_metrics(lcf, "/metrics?__name__=comp*", "gzip, zstd");
        BOOST_REQUIRE_EQUAL(reply.content_encoding, "");
        BOOST_REQUIRE_EQUAL(reply.vary, "");
        BOOST_REQUIRE(std::ranges::search(reply.body, "seastar_comp_family_0"sv));
    });
}

// A compressed reply decompresses to the uncompressed one, in both formats.
// One family holds all the series, so that the protobuf format, which writes a
// family at once, passes the compressor a buffer much larger than it compresses
// in one go.
SEASTAR_TEST_CASE(test_prometheus_compression) {
    auto metrics = make_series(20000, 20000);
    prometheus::config ctx;
    ctx.allow_protobuf = true;
    for (auto& coding : supported_codings()) {
        ctx.compression.push_back(coding.type);
    }
    co_await with_prometheus_server(ctx, [] (loopback_connection_factory& lcf) {
        const sstring path = "/metrics?__name__=comp*";
        for (auto& accept : {sstring(), accept_protobuf}) {
            auto plain = get_metrics(lcf, path, "", accept);
            BOOST_REQUIRE_EQUAL(plain.content_encoding, "");
            // In a build that supports no coding, nothing is enabled
            BOOST_REQUIRE_EQUAL(plain.vary, supported_codings().empty() ? "" : "Accept-Encoding");
            BOOST_REQUIRE_GT(plain.body.size(), 500'000u);

            for (auto& coding : supported_codings()) {
                auto reply = get_metrics(lcf, path, coding.name, accept);
                BOOST_REQUIRE_EQUAL(reply.content_encoding, coding.name);
                BOOST_REQUIRE_EQUAL(reply.vary, "Accept-Encoding");
                BOOST_REQUIRE_LT(reply.body.size(), plain.body.size() / 2);
                BOOST_REQUIRE(coding.decompress(reply.body) == plain.body);
            }
        }
#if defined(SEASTAR_HAVE_ZLIB) && defined(SEASTAR_HAVE_ZSTD)
        BOOST_REQUIRE_EQUAL(get_metrics(lcf, path, "gzip, zstd").content_encoding, "zstd");
        BOOST_REQUIRE_EQUAL(get_metrics(lcf, path, "zstd;q=0, gzip").content_encoding, "gzip");
#endif
        BOOST_REQUIRE_EQUAL(get_metrics(lcf, path, "br").content_encoding, "");
    });
}

// A coding that Seastar was built without cannot be enabled
SEASTAR_TEST_CASE(test_prometheus_compression_not_built) {
    return seastar::async([] {
        http_server server("test");
        auto stop_server = defer([&server] () noexcept { server.stop().get(); });
        prometheus::config ctx;
        ctx.compression = {prometheus::compression_type::gzip};
#ifdef SEASTAR_HAVE_ZLIB
        add_prometheus_routes(server, ctx).get();
#else
        BOOST_REQUIRE_THROW(add_prometheus_routes(server, ctx).get(), std::invalid_argument);
#endif
        ctx.compression = {prometheus::compression_type::zstd};
#ifndef SEASTAR_HAVE_ZSTD
        BOOST_REQUIRE_THROW(add_prometheus_routes(server, ctx).get(), std::invalid_argument);
#endif
    });
}

// Scrapes a huge number of metrics with every coding and in both formats,
// checks that each compressed reply decompresses to the uncompressed one, and
// reports the size of the reply, the time the scrape took, the number of
// reactor stalls during it and the longest of them. A compressed scrape must not stall the reactor
// more than the uncompressed one does.
//
// It takes a lot of memory and time, so it does not run by default:
//
//   prometheus_http_test --run_test=test_prometheus_compression_stress -- -c1
//
// The environment sets its size and its threshold:
//   PROMETHEUS_STRESS_SERIES       number of series (default 1000000)
//   PROMETHEUS_STRESS_FAMILY_SIZE  number of series in a family (default 1000)
//   PROMETHEUS_STRESS_STALL_MS     what counts as a stall, in ms (default 2)
SEASTAR_TEST_CASE(test_prometheus_compression_stress, *boost::unit_test::disabled()) {
    auto env = [] (const char* name, size_t def) {
        auto* v = std::getenv(name);
        return v ? std::strtoull(v, nullptr, 10) : def;
    };
    const size_t series = env("PROMETHEUS_STRESS_SERIES", 1'000'000);
    const size_t family_size = env("PROMETHEUS_STRESS_FAMILY_SIZE", 1000);
    const auto threshold = std::chrono::milliseconds(env("PROMETHEUS_STRESS_STALL_MS", 2));

    auto metrics = make_series(series, family_size);
    prometheus::config ctx;
    ctx.allow_protobuf = true;
    for (auto& coding : supported_codings()) {
        ctx.compression.push_back(coding.type);
    }

    co_await with_prometheus_server(ctx, [&] (loopback_connection_factory& lcf) {
        // The stall detector limits the rate of its reports, so it cannot count
        // stalls. Instead, a fiber that does nothing but yield measures how long
        // the reactor takes to get back to it.
        unsigned stalls = 0;
        std::chrono::steady_clock::duration longest{};
        uint64_t samples = 0;
        bool stop = false;
        // Takes its state as parameters: a coroutine lambda's captures die with
        // the lambda, before the coroutine is done.
        auto probe = [] (bool& stop, unsigned& stalls, std::chrono::steady_clock::duration& longest, uint64_t& samples,
                std::chrono::milliseconds threshold) -> future<> {
            while (!stop) {
                auto start = std::chrono::steady_clock::now();
                co_await yield();
                auto gap = std::chrono::steady_clock::now() - start;
                stalls += gap > threshold;
                longest = std::max(longest, gap);
                samples++;
            }
        }(stop, stalls, longest, samples, threshold);
        // Waits for the probe to complete a sample. On return, the probe has
        // accounted for all the time until now, and started a new sample.
        auto sync_probe = [&] {
            auto seen = samples;
            while (samples == seen) {
                thread::yield();
            }
        };
        auto stop_probe = defer([&] () noexcept {
            stop = true;
            probe.get();
        });

        struct result {
            metrics_reply reply;
            unsigned stalls;
        };
        // The stalls are counted over the request only: copying, decompressing
        // and comparing the bodies stall the reactor on their own. So the
        // probe is synchronized with both ends of the request.
        auto scrape = [&] (const sstring& format, const sstring& accept, const sstring& coding) {
            sync_probe();
            stalls = 0;
            longest = {};
            unsigned request_stalls = 0;
            std::chrono::steady_clock::duration request_longest{}, elapsed{};
            auto start = std::chrono::steady_clock::now();
            auto reply = get_metrics(lcf, "/metrics?__name__=comp*", coding, accept, [&] {
                elapsed = std::chrono::steady_clock::now() - start;
                sync_probe();
                request_stalls = stalls;
                request_longest = longest;
            });
            result ret{std::move(reply), request_stalls};
            auto ms = [] (auto d) { return std::chrono::duration<double, std::milli>(d).count(); };
            fmt::print("{:8} {:8} {:>12} bytes {:>8.1f} ms, {:>3} stalls over {} ms, longest {:.2f} ms\n",
                    format, coding.empty() ? "identity" : coding, ret.reply.body.size(), ms(elapsed), ret.stalls, threshold.count(), ms(request_longest));
            return ret;
        };

        fmt::print("{} series in families of {}\n", series, family_size);
        for (auto& [format, accept] : {std::pair<sstring, sstring>{"text", ""}, {"protobuf", accept_protobuf}}) {
            auto plain = scrape(format, accept, "");
            BOOST_REQUIRE_EQUAL(plain.reply.content_encoding, "");
            for (auto& coding : supported_codings()) {
                auto compressed = scrape(format, accept, coding.name);
                BOOST_REQUIRE_EQUAL(compressed.reply.content_encoding, coding.name);
                BOOST_CHECK_MESSAGE(compressed.stalls <= plain.stalls,
                        fmt::format("{} {}: {} stalls, {} without compression", format, coding.name, compressed.stalls, plain.stalls));
                BOOST_REQUIRE(coding.decompress(compressed.reply.body) == plain.reply.body);
            }
        }
    });
}
