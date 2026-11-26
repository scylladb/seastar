/*
 * This file is open source software, licensed to you under the terms
 * of the Apache License, Version 2.0 (the "License").  See the NOTICE file
 * distributed with this work for additional information regarding copyright
 * ownership.  You may not use this file except in compliance with the License.
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
 * Copyright (C) 2025 ScyllaDB
 */

// Tests for the protobuf exposition format.
//
// write_protobuf_representation() reuses a single MetricFamily and a single
// std::string buffer across every family in a scrape, and add_label() assigns
// into the retained strings. Nothing else in the tree decodes protobuf output,
// so these tests parse the delimited stream and check the fields directly - in
// particular that neither metrics nor labels nor typed payloads from one family
// leak into the next.

#include <boost/test/tools/old/interface.hpp>
#include <seastar/core/internal/estimated_histogram.hh>
#include <seastar/core/iostream.hh>
#include <seastar/core/metrics.hh>
#include <seastar/core/metrics_api.hh>
#include <seastar/core/metrics_registration.hh>
#include <seastar/core/prometheus.hh>
#include <seastar/testing/test_case.hh>

#include "memory-data-sink.hh"
#include "proto/metrics2.pb.h"

#include <cmath>
#include <cstdint>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace pm = io::prometheus::client;

using namespace seastar;
using namespace seastar::prometheus;

namespace sm = seastar::metrics;
namespace mi = sm::impl;
namespace sp = seastar::prometheus;

namespace {

[[maybe_unused]] void remove_existing_metrics() {
    // Unregistering invalidates iterators into the value map, so collect the
    // metric ids first and only then remove them.
    std::vector<mi::metric_id> ids;
    for (const auto& family : sm::impl::get_value_map()) {
        for (const auto& series : family.second) {
            if (series.second) {
                ids.push_back(series.second->get_id());
            }
        }
    }
    for (const auto& id : ids) {
        sm::impl::unregister_metric(id);
    }
}

const sp::details::filter_t keep_all = [](const mi::labels_type&) { return true; };

const sp::details::family_filter_t keep_all_families = [](std::string_view) { return true; };

// Parse the varint-delimited MetricFamily stream emitted by the renderer.
std::vector<pm::MetricFamily> parse_delimited(std::string_view data) {
    std::vector<pm::MetricFamily> families;
    size_t pos = 0;
    while (pos < data.size()) {
        uint64_t len = 0;
        unsigned shift = 0;
        for (;;) {
            BOOST_REQUIRE_LT(pos, data.size());
            const auto byte = static_cast<uint8_t>(data[pos++]);
            len |= uint64_t(byte & 0x7f) << shift;
            if (!(byte & 0x80)) {
                break;
            }
            shift += 7;
            BOOST_REQUIRE_LT(shift, 64u);
        }
        BOOST_REQUIRE_LE(pos + len, data.size());
        pm::MetricFamily mf;
        BOOST_REQUIRE(mf.ParseFromArray(data.data() + pos, static_cast<int>(len)));
        pos += len;
        families.push_back(std::move(mf));
    }
    return families;
}

std::map<std::string, pm::MetricFamily> families_by_name(std::vector<pm::MetricFamily> families) {
    std::map<std::string, pm::MetricFamily> out;
    for (auto& mf : families) {
        out.emplace(mf.name(), std::move(mf));
    }
    return out;
}

std::map<std::string, std::string> labels_of(const pm::Metric& m) {
    std::map<std::string, std::string> out;
    for (const auto& l : m.label()) {
        out[l.name()] = l.value();
    }
    return out;
}

constexpr uint64_t histo_min = 1, histo_max = 1000000;
using histo_type = sm::internal::approximate_exponential_histogram<histo_min, histo_max, 1>;

auto make_test_histogram() {
    auto histogram = std::make_shared<histo_type>();
    for (double v = histo_min; v < histo_max; v *= 1.3) {
        histogram->add(v);
    }
    return histogram;
}

} // namespace

struct prometheus_test_fixture {
    static future<> remove_metrics() {
        return smp::invoke_on_all([] {
            remove_existing_metrics();
        });
    }

    static future<std::string> render_protobuf(sp::config cfg, bool enable_aggregation = false) {
        using access = sp::details::test_access;
        std::stringstream ss;
        output_stream<char> out{data_sink{std::make_unique<testing::memory_data_sink_impl>(ss, 10)}};
        co_await access{}.write_body(std::move(cfg), sp::details::write_body_args{
            .filter = keep_all,
            .family_filter = keep_all_families,
            .use_protobuf_format = true,
            .show_help = true,
            .enable_aggregation = enable_aggregation,
        }, std::move(out));
        co_return ss.str();
    }
};

SEASTAR_TEST_CASE(test_protobuf_counter_with_global_label) {
    co_await prometheus_test_fixture::remove_metrics();

    std::vector<sm::metric_definition> defs;
    defs.emplace_back(sm::make_counter("requests", sm::description("number of requests"),
            {sm::label("env")("prod")}, [] { return 123; }));
    sm::metric_groups groups;
    groups.add_group("test", defs);

    sp::config cfg;
    cfg.label = sm::label("instance")("verify-instance");

    const auto families = families_by_name(parse_delimited(
        co_await prometheus_test_fixture::render_protobuf(std::move(cfg))));

    BOOST_REQUIRE_EQUAL(families.size(), 1);
    const auto& mf = families.at("seastar_test_requests");
    BOOST_REQUIRE_EQUAL(mf.type(), pm::COUNTER);
    BOOST_REQUIRE_EQUAL(mf.metric_size(), 1);

    const auto& metric = mf.metric(0);
    BOOST_REQUIRE(metric.has_counter());
    BOOST_REQUIRE_EQUAL(metric.counter().value(), 123);

    // The global label is added by add_label() before the metric's own labels.
    const auto labels = labels_of(metric);
    BOOST_REQUIRE_EQUAL(labels.size(), 3);
    BOOST_REQUIRE_EQUAL(labels.at("instance"), "verify-instance");
    BOOST_REQUIRE_EQUAL(labels.at("env"), "prod");
    BOOST_REQUIRE_EQUAL(labels.at("shard"), "0");
}

SEASTAR_TEST_CASE(test_protobuf_does_not_leak_metrics_between_families) {
    co_await prometheus_test_fixture::remove_metrics();

    std::vector<sm::metric_definition> defs;
    for (int i = 0; i < 3; ++i) {
        defs.emplace_back(sm::make_counter("metric_" + std::to_string(i),
                sm::description("desc_" + std::to_string(i)),
                {sm::label("which")("m" + std::to_string(i))}, [] { return 7; }));
    }
    sm::metric_groups groups;
    groups.add_group("test", defs);

    const auto families = families_by_name(parse_delimited(
        co_await prometheus_test_fixture::render_protobuf({})));

    BOOST_REQUIRE_EQUAL(families.size(), 3);
    for (int i = 0; i < 3; ++i) {
        const auto& mf = families.at("seastar_test_metric_" + std::to_string(i));
        BOOST_REQUIRE_EQUAL(mf.type(), pm::COUNTER);
        // The reused message must start from an empty family: if Clear() were
        // missing, later families would accumulate the earlier metrics.
        BOOST_REQUIRE_EQUAL(mf.metric_size(), 1);
        const auto labels = labels_of(mf.metric(0));
        BOOST_REQUIRE_EQUAL(labels.size(), 2);
        BOOST_REQUIRE_EQUAL(labels.at("which"), "m" + std::to_string(i));
        BOOST_REQUIRE_EQUAL(labels.at("shard"), "0");
        BOOST_REQUIRE_EQUAL(mf.metric(0).counter().value(), 7);
    }
}

SEASTAR_TEST_CASE(test_protobuf_does_not_leak_typed_payload_between_families) {
    co_await prometheus_test_fixture::remove_metrics();

    std::vector<sm::metric_definition> defs;
    defs.emplace_back(sm::make_histogram("a_hist", sm::description("a histogram"),
            {sm::label("kind")("hist")},
            [histogram = make_test_histogram()] { return histogram->to_metrics_histogram(); }));
    defs.emplace_back(sm::make_gauge("b_gauge", sm::description("a gauge"),
            {sm::label("kind")("gauge")}, [] { return 123.4; }));
    sm::metric_groups groups;
    groups.add_group("test", defs);

    const auto families = families_by_name(parse_delimited(
        co_await prometheus_test_fixture::render_protobuf({})));
    BOOST_REQUIRE_EQUAL(families.size(), 2);

    const auto& hist = families.at("seastar_test_a_hist");
    BOOST_REQUIRE_EQUAL(hist.type(), pm::HISTOGRAM);
    BOOST_REQUIRE_EQUAL(hist.metric_size(), 1);
    const auto& hm = hist.metric(0);
    BOOST_REQUIRE(hm.has_histogram());
    BOOST_REQUIRE(!hm.has_gauge());
    BOOST_REQUIRE_EQUAL(hm.histogram().sample_count(), 53);
    BOOST_REQUIRE_CLOSE(hm.histogram().sample_sum(), 6.42072e6, 0.01);
    BOOST_REQUIRE(hm.histogram().positive_span_size() + hm.histogram().bucket_size() > 0);

    const auto& gauge = families.at("seastar_test_b_gauge");
    BOOST_REQUIRE_EQUAL(gauge.type(), pm::GAUGE);
    BOOST_REQUIRE_EQUAL(gauge.metric_size(), 1);
    const auto& gm = gauge.metric(0);
    BOOST_REQUIRE(gm.has_gauge());
    // The gauge must not inherit the histogram payload (or its labels) from the
    // family rendered before it.
    BOOST_REQUIRE(!gm.has_histogram());
    BOOST_REQUIRE_CLOSE(gm.gauge().value(), 123.4, 0.001);
    const auto labels = labels_of(gm);
    BOOST_REQUIRE_EQUAL(labels.size(), 2);
    BOOST_REQUIRE_EQUAL(labels.at("kind"), "gauge");
}

SEASTAR_TEST_CASE(test_protobuf_aggregation) {
    co_await prometheus_test_fixture::remove_metrics();

    std::vector<sm::metric_definition> defs;
    for (int i = 0; i < 2; ++i) {
        sm::impl::metric_definition_impl impl = sm::make_counter("requests",
                sm::description("number of requests"),
                {sm::label("which")("m" + std::to_string(i))}, [] { return 100; });
        impl.aggregate({sm::label("which")});
        defs.emplace_back(std::move(impl));
    }
    sm::metric_groups groups;
    groups.add_group("test", defs);

    const auto families = families_by_name(parse_delimited(
        co_await prometheus_test_fixture::render_protobuf({}, /*enable_aggregation=*/true)));

    BOOST_REQUIRE_EQUAL(families.size(), 1);
    const auto& mf = families.at("seastar_test_requests");
    BOOST_REQUIRE_EQUAL(mf.metric_size(), 1);
    const auto& metric = mf.metric(0);
    BOOST_REQUIRE(metric.has_counter());
    BOOST_REQUIRE_EQUAL(metric.counter().value(), 200);
    // The aggregated-away label is gone; the common labels remain.
    const auto labels = labels_of(metric);
    BOOST_REQUIRE_EQUAL(labels.count("which"), 0);
    BOOST_REQUIRE_EQUAL(labels.at("shard"), "0");
}
