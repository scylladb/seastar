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

#include <boost/test/tools/old/interface.hpp>
#include <cstddef>
#include <seastar/core/internal/estimated_histogram.hh>
#include <seastar/core/iostream.hh>
#include <seastar/core/metrics.hh>
#include <seastar/core/metrics_api.hh>
#include <seastar/core/metrics_registration.hh>
#include <seastar/core/prometheus.hh>
#include <seastar/core/sleep.hh>
#include <seastar/testing/test_case.hh>
#include <seastar/util/closeable.hh>

#include "core/prometheus-impl.hh"
#include "memory-data-sink.hh"

#include <sstream>
#include <string_view>


using namespace seastar;
using namespace httpd;
using namespace std::literals;
using namespace seastar::prometheus;

namespace sm = seastar::metrics;
namespace sp = seastar::prometheus;
namespace mi = sm::impl;

using labels_list_type = std::vector<std::string>;

thread_local auto impl_ = sm::impl::get_local_impl();

namespace {
[[maybe_unused]] void remove_existing_metrics() {
    // Unregistering invalidates iterators into the value map, so collect
    // the metric ids first and only then remove them.
    std::vector<mi::metric_id> ids;
    for (const auto& family : seastar::metrics::impl::get_value_map()) {
        for (const auto& series : family.second) {
            if (series.second) {
                ids.push_back(series.second->get_id());
            }
        }
    }
    for (const auto& id : ids) {
        seastar::metrics::impl::unregister_metric(id);
    }

    assert(seastar::metrics::impl::get_value_map().size() == 0);
}

}

using data_type = seastar::metrics::impl::data_type;

static const sp::details::filter_t always_true = [](auto& mi){ return true; };

enum class aggr_mode {
    NO_AGGR,
    AGGR_LABEL_0,
    AGGR_SHARD_LABEL,
};

struct test_config {
    data_type type;
    // number of metrics to create
    size_t count = 1;
    // number of labels on each metric
    size_t labels_per_metric = 1;
    std::optional<sp::details::filter_t> filter;
    std::optional<sp::details::family_filter_t> family_filter;
    bool show_help = true;
    aggr_mode aggregation_mode = aggr_mode::NO_AGGR;
    bool same_metric_name = false;
    std::optional<sm::label_instance> extra_label;
};

static constexpr uint64_t histo_min = 1, histo_max = 1000000;
using histo_type = sm::internal::approximate_exponential_histogram<histo_min, histo_max, 1>;

auto make_historgam() {
    auto histogram = std::make_shared<histo_type>();
    for (double v = histo_min; v < histo_max; v *= 1.3) {
        histogram->add(v);
    }
    return histogram;
}

struct prometheus_test_fixture {
    static constexpr uint64_t histo_min = 1, histo_max = 1000000;
    using histo_type = sm::internal::approximate_exponential_histogram<histo_min, histo_max, 1>;
    const int histo_buckets = histo_type{}.find_bucket_index(-1) + 1;

    static constexpr size_t name_length = 10;

    static seastar::future<> run_metrics_test(test_config test_conf, prometheus::config config, std::string_view expected) {

        co_await smp::invoke_on_all([] {
            remove_existing_metrics();
        });

        sm::metric_groups test_metrics;

        auto nth_label = [](size_t n) {
            return sm::label(fmt::format("label-{}", n));
        };

        std::vector<sm::metric_definition> defs;

        auto desc = "metric description";

        for (size_t i = 0; i < test_conf.count; ++i) {
            auto metric_name = test_conf.same_metric_name ? "metric" : fmt::format("metric_{}", i);
            std::vector<sm::label_instance> labels;

            for (size_t label_idx = 0; label_idx < test_conf.labels_per_metric; ++label_idx) {
                auto label_value = fmt::format("label-{}-{}", label_idx, i);
                labels.push_back(nth_label(label_idx)(label_value));
            }

            if (test_conf.extra_label) {
                labels.push_back(*test_conf.extra_label);
            }

            sm::impl::metric_definition_impl impl = [&] {
                if (test_conf.type == data_type::COUNTER) {
                    return
                    sm::make_counter(metric_name, sm::description(desc), labels, [] { return 123; });
                } else if (test_conf.type == data_type::REAL_COUNTER) {
                    return
                    sm::make_counter(metric_name, sm::description(desc), labels, [] { return 123.4; });
                } else if (test_conf.type == data_type::GAUGE) {
                    return
                    sm::make_gauge(metric_name, sm::description(desc), labels, [] { return 123.4; });
                } else if (test_conf.type == data_type::HISTOGRAM) {
                    return make_histogram(metric_name, sm::description(metric_name), labels,
                            [histogram = make_historgam()]() { return histogram->to_metrics_histogram(); });
                } else if (test_conf.type == data_type::SUMMARY) {
                    // SUMMARY doesn't support specifying labels
                    return make_summary(metric_name, sm::description(metric_name),
                            [histogram = make_historgam()]() { return histogram->to_metrics_histogram(); });
                }
                BOOST_FAIL("unknown data type");
                __builtin_unreachable();
            }();

            if (test_conf.aggregation_mode == aggr_mode::AGGR_LABEL_0) {
                impl.aggregate({nth_label(0)});
            } else if (test_conf.aggregation_mode == aggr_mode::AGGR_SHARD_LABEL) {
                impl.aggregate({sm::shard_label});
            }

            defs.emplace_back(impl);

        }

        test_metrics.add_group(fmt::format("group-{}", 1), defs);

        auto filter = test_conf.filter.value_or(always_true);
        auto family_filter = test_conf.family_filter.value_or([](std::string_view) { return true; });
        auto args = [&] (std::optional<sp::details::filter_key> cache_key) {
            return sp::details::write_body_args{
                .filter = filter,
                .family_filter = family_filter,
                .use_protobuf_format = false,
                .show_help = test_conf.show_help,
                .enable_aggregation = test_conf.aggregation_mode != aggr_mode::NO_AGGR,
                .cache_key = std::move(cache_key),
            };
        };
        auto check = [&] (std::string_view what, sstring actual) {
            BOOST_REQUIRE_MESSAGE(expected == actual,
                fmt::format("{}: actual output doesn't match expected\nexpected output:\n{}\nactual output:\n{}",
                what, expected, actual));
        };

        using access = prometheus::details::test_access;
        access::clear_cache();

        // Without caching
        check("uncached", co_await scrape(config, args(std::nullopt)));
        BOOST_REQUIRE_EQUAL(access::cache_stats().entries, 0);

        // With caching: the first request builds the template, the
        // following ones use it
        check("cache miss", co_await scrape(config, args(sp::details::filter_key{})));
        for (int i = 0; i < 2; ++i) {
            check("cache hit", co_await scrape(config, args(sp::details::filter_key{})));
        }
        auto stats = access::cache_stats();
        BOOST_REQUIRE_EQUAL(stats.builds, 2);
        BOOST_REQUIRE_EQUAL(stats.hits, 2);
        BOOST_REQUIRE_EQUAL(stats.invalidations, 0);
        BOOST_REQUIRE_EQUAL(stats.entries, 1);
    }

    static sp::details::text_cache_stats cache_stats() {
        return sp::details::test_access::cache_stats();
    }
    static void clear_cache() {
        sp::details::test_access::clear_cache();
    }
    static void set_cache_ttl(std::chrono::milliseconds ttl) {
        sp::details::test_access::set_cache_ttl(ttl);
    }
    static const void* cached_template() {
        return sp::details::test_access::cached_template();
    }
    static void set_numa_node_mapping(std::optional<std::vector<unsigned>> mapping) {
        sp::details::test_access::set_numa_node_mapping(std::move(mapping));
    }

    static seastar::future<sstring> scrape(prometheus::config config, sp::details::write_body_args args) {
        std::stringstream ss;
        output_stream<char> out{data_sink{std::make_unique<testing::memory_data_sink_impl>(ss, 10)}};
        co_await prometheus::details::test_access{}.write_body(config, std::move(args), std::move(out));
        co_return sstring(ss.str());
    }
};

// Arguments for a request without filters
static sp::details::write_body_args all_metrics(std::optional<sp::details::filter_key> cache_key = sp::details::filter_key{}) {
    return {
        .filter = always_true,
        .family_filter = [](std::string_view) { return true; },
        .use_protobuf_format = false,
        .show_help = false,
        .enable_aggregation = true,
        .cache_key = std::move(cache_key),
    };
}

SEASTAR_TEST_CASE(test_basic_counter) {
    test_config cfg{data_type::COUNTER};
    return prometheus_test_fixture::run_metrics_test(cfg, {},
        R"(# HELP seastar_group_1_metric_0 metric description)" "\n"
        R"(# TYPE seastar_group_1_metric_0 counter)" "\n"
        R"(seastar_group_1_metric_0{label-0="label-0-0",shard="0"} 123)" "\n"
    );
}

SEASTAR_TEST_CASE(test_basic_counter_no_labels) {
    test_config cfg{data_type::COUNTER};
    cfg.labels_per_metric = 0;
    cfg.aggregation_mode = aggr_mode::AGGR_SHARD_LABEL;
    return prometheus_test_fixture::run_metrics_test(cfg, {},
        R"(# HELP seastar_group_1_metric_0 metric description)" "\n"
        R"(# TYPE seastar_group_1_metric_0 counter)" "\n"
        R"(seastar_group_1_metric_0{} 123)" "\n"
    );
}

SEASTAR_TEST_CASE(test_histo_no_labels) {
    // important case b/c of how we inject the le label, which could
    // fail in the case there are no other labels
    test_config cfg{data_type::HISTOGRAM};
    cfg.labels_per_metric = 0;
    cfg.aggregation_mode = aggr_mode::AGGR_SHARD_LABEL;
    return prometheus_test_fixture::run_metrics_test(cfg, {},
        R"(# HELP seastar_group_1_metric_0 metric_0)" "\n"
        R"(# TYPE seastar_group_1_metric_0 histogram)" "\n"
        R"(seastar_group_1_metric_0_sum{} 6.42072e+06)" "\n"
        R"(seastar_group_1_metric_0_count{} 53)" "\n"
        R"(seastar_group_1_metric_0_bucket{le="2.000000"} 3)" "\n"
        R"(seastar_group_1_metric_0_bucket{le="4.000000"} 6)" "\n"
        R"(seastar_group_1_metric_0_bucket{le="8.000000"} 8)" "\n"
        R"(seastar_group_1_metric_0_bucket{le="16.000000"} 11)" "\n"
        R"(seastar_group_1_metric_0_bucket{le="32.000000"} 14)" "\n"
        R"(seastar_group_1_metric_0_bucket{le="64.000000"} 16)" "\n"
        R"(seastar_group_1_metric_0_bucket{le="128.000000"} 19)" "\n"
        R"(seastar_group_1_metric_0_bucket{le="256.000000"} 22)" "\n"
        R"(seastar_group_1_metric_0_bucket{le="512.000000"} 24)" "\n"
        R"(seastar_group_1_metric_0_bucket{le="1024.000000"} 27)" "\n"
        R"(seastar_group_1_metric_0_bucket{le="2048.000000"} 30)" "\n"
        R"(seastar_group_1_metric_0_bucket{le="4096.000000"} 32)" "\n"
        R"(seastar_group_1_metric_0_bucket{le="8192.000000"} 35)" "\n"
        R"(seastar_group_1_metric_0_bucket{le="16384.000000"} 37)" "\n"
        R"(seastar_group_1_metric_0_bucket{le="32768.000000"} 40)" "\n"
        R"(seastar_group_1_metric_0_bucket{le="65536.000000"} 43)" "\n"
        R"(seastar_group_1_metric_0_bucket{le="131072.000000"} 45)" "\n"
        R"(seastar_group_1_metric_0_bucket{le="262144.000000"} 48)" "\n"
        R"(seastar_group_1_metric_0_bucket{le="1000000.000000"} 51)" "\n"
        R"(seastar_group_1_metric_0_bucket{le="+Inf"} 53)" "\n"
    );
}

SEASTAR_TEST_CASE(test_basic_gauge) {
    test_config cfg{data_type::GAUGE};
    return prometheus_test_fixture::run_metrics_test(cfg, {},
        R"(# HELP seastar_group_1_metric_0 metric description)" "\n"
        R"(# TYPE seastar_group_1_metric_0 gauge)" "\n"
        R"(seastar_group_1_metric_0{label-0="label-0-0",shard="0"} 123.400000)" "\n"
    );
}

SEASTAR_TEST_CASE(test_basic_histogram) {
    test_config cfg{data_type::HISTOGRAM};
    return prometheus_test_fixture::run_metrics_test(cfg, {},
        R"(# HELP seastar_group_1_metric_0 metric_0)" "\n"
        R"(# TYPE seastar_group_1_metric_0 histogram)" "\n"
        R"(seastar_group_1_metric_0_sum{label-0="label-0-0",shard="0"} 6.42072e+06)" "\n"
        R"(seastar_group_1_metric_0_count{label-0="label-0-0",shard="0"} 53)" "\n"
        R"(seastar_group_1_metric_0_bucket{label-0="label-0-0",le="2.000000",shard="0"} 3)" "\n"
        R"(seastar_group_1_metric_0_bucket{label-0="label-0-0",le="4.000000",shard="0"} 6)" "\n"
        R"(seastar_group_1_metric_0_bucket{label-0="label-0-0",le="8.000000",shard="0"} 8)" "\n"
        R"(seastar_group_1_metric_0_bucket{label-0="label-0-0",le="16.000000",shard="0"} 11)" "\n"
        R"(seastar_group_1_metric_0_bucket{label-0="label-0-0",le="32.000000",shard="0"} 14)" "\n"
        R"(seastar_group_1_metric_0_bucket{label-0="label-0-0",le="64.000000",shard="0"} 16)" "\n"
        R"(seastar_group_1_metric_0_bucket{label-0="label-0-0",le="128.000000",shard="0"} 19)" "\n"
        R"(seastar_group_1_metric_0_bucket{label-0="label-0-0",le="256.000000",shard="0"} 22)" "\n"
        R"(seastar_group_1_metric_0_bucket{label-0="label-0-0",le="512.000000",shard="0"} 24)" "\n"
        R"(seastar_group_1_metric_0_bucket{label-0="label-0-0",le="1024.000000",shard="0"} 27)" "\n"
        R"(seastar_group_1_metric_0_bucket{label-0="label-0-0",le="2048.000000",shard="0"} 30)" "\n"
        R"(seastar_group_1_metric_0_bucket{label-0="label-0-0",le="4096.000000",shard="0"} 32)" "\n"
        R"(seastar_group_1_metric_0_bucket{label-0="label-0-0",le="8192.000000",shard="0"} 35)" "\n"
        R"(seastar_group_1_metric_0_bucket{label-0="label-0-0",le="16384.000000",shard="0"} 37)" "\n"
        R"(seastar_group_1_metric_0_bucket{label-0="label-0-0",le="32768.000000",shard="0"} 40)" "\n"
        R"(seastar_group_1_metric_0_bucket{label-0="label-0-0",le="65536.000000",shard="0"} 43)" "\n"
        R"(seastar_group_1_metric_0_bucket{label-0="label-0-0",le="131072.000000",shard="0"} 45)" "\n"
        R"(seastar_group_1_metric_0_bucket{label-0="label-0-0",le="262144.000000",shard="0"} 48)" "\n"
        R"(seastar_group_1_metric_0_bucket{label-0="label-0-0",le="1000000.000000",shard="0"} 51)" "\n"
        R"(seastar_group_1_metric_0_bucket{label-0="label-0-0",le="+Inf",shard="0"} 53)" "\n"
    );
}

SEASTAR_TEST_CASE(test_basic_summary) {
    test_config cfg{data_type::SUMMARY};
    return prometheus_test_fixture::run_metrics_test(cfg, {},
        R"(# HELP seastar_group_1_metric_0 metric_0)" "\n"
        R"(# TYPE seastar_group_1_metric_0 summary)" "\n"
        R"(seastar_group_1_metric_0_sum{shard="0"} 6.42072e+06)" "\n"
        R"(seastar_group_1_metric_0_count{shard="0"} 53)" "\n"
        R"(seastar_group_1_metric_0{quantile="2.000000",shard="0"} 3)" "\n"
        R"(seastar_group_1_metric_0{quantile="4.000000",shard="0"} 6)" "\n"
        R"(seastar_group_1_metric_0{quantile="8.000000",shard="0"} 8)" "\n"
        R"(seastar_group_1_metric_0{quantile="16.000000",shard="0"} 11)" "\n"
        R"(seastar_group_1_metric_0{quantile="32.000000",shard="0"} 14)" "\n"
        R"(seastar_group_1_metric_0{quantile="64.000000",shard="0"} 16)" "\n"
        R"(seastar_group_1_metric_0{quantile="128.000000",shard="0"} 19)" "\n"
        R"(seastar_group_1_metric_0{quantile="256.000000",shard="0"} 22)" "\n"
        R"(seastar_group_1_metric_0{quantile="512.000000",shard="0"} 24)" "\n"
        R"(seastar_group_1_metric_0{quantile="1024.000000",shard="0"} 27)" "\n"
        R"(seastar_group_1_metric_0{quantile="2048.000000",shard="0"} 30)" "\n"
        R"(seastar_group_1_metric_0{quantile="4096.000000",shard="0"} 32)" "\n"
        R"(seastar_group_1_metric_0{quantile="8192.000000",shard="0"} 35)" "\n"
        R"(seastar_group_1_metric_0{quantile="16384.000000",shard="0"} 37)" "\n"
        R"(seastar_group_1_metric_0{quantile="32768.000000",shard="0"} 40)" "\n"
        R"(seastar_group_1_metric_0{quantile="65536.000000",shard="0"} 43)" "\n"
        R"(seastar_group_1_metric_0{quantile="131072.000000",shard="0"} 45)" "\n"
        R"(seastar_group_1_metric_0{quantile="262144.000000",shard="0"} 48)" "\n"
        R"(seastar_group_1_metric_0{quantile="1000000.000000",shard="0"} 51)" "\n"
    );
}

SEASTAR_TEST_CASE(test_counter_with_custom_prefix) {
    test_config cfg{data_type::COUNTER};
    prometheus::config prom_cfg;
    prom_cfg.prefix = "myapp";
    return prometheus_test_fixture::run_metrics_test(cfg, prom_cfg,
        R"(# HELP myapp_group_1_metric_0 metric description)" "\n"
        R"(# TYPE myapp_group_1_metric_0 counter)" "\n"
        R"(myapp_group_1_metric_0{label-0="label-0-0",shard="0"} 123)" "\n"
    );
}

SEASTAR_TEST_CASE(test_counter_with_label) {
    test_config cfg{data_type::COUNTER};
    prometheus::config prom_cfg;
    sm::label extra_label{"env"};
    prom_cfg.label = extra_label("production");
    return prometheus_test_fixture::run_metrics_test(cfg, prom_cfg,
        R"(# HELP seastar_group_1_metric_0 metric description)" "\n"
        R"(# TYPE seastar_group_1_metric_0 counter)" "\n"
        R"(seastar_group_1_metric_0{env="production",label-0="label-0-0",shard="0"} 123)" "\n"
    );
}

SEASTAR_TEST_CASE(test_multiple_counters) {
    test_config cfg{data_type::COUNTER, 3};
    return prometheus_test_fixture::run_metrics_test(cfg, {},
        R"(# HELP seastar_group_1_metric_0 metric description)" "\n"
        R"(# TYPE seastar_group_1_metric_0 counter)" "\n"
        R"(seastar_group_1_metric_0{label-0="label-0-0",shard="0"} 123)" "\n"
        R"(# HELP seastar_group_1_metric_1 metric description)" "\n"
        R"(# TYPE seastar_group_1_metric_1 counter)" "\n"
        R"(seastar_group_1_metric_1{label-0="label-0-1",shard="0"} 123)" "\n"
        R"(# HELP seastar_group_1_metric_2 metric description)" "\n"
        R"(# TYPE seastar_group_1_metric_2 counter)" "\n"
        R"(seastar_group_1_metric_2{label-0="label-0-2",shard="0"} 123)" "\n"
    );
}

SEASTAR_TEST_CASE(test_metrics_filtering) {
    // Filter to exclude the first metric (label-0="label-0-0")
    // This should return only metric_1 and metric_2
    sp::details::filter_t filter = [](const sm::impl::labels_type& labels) {
        auto it = labels.find("label-0");
        return it == labels.end() || it->second.value() != "label-0-0";
    };

    test_config cfg{data_type::COUNTER, 3};
    cfg.filter = filter;
    return prometheus_test_fixture::run_metrics_test(cfg, {},
        R"(# HELP seastar_group_1_metric_1 metric description)" "\n"
        R"(# TYPE seastar_group_1_metric_1 counter)" "\n"
        R"(seastar_group_1_metric_1{label-0="label-0-1",shard="0"} 123)" "\n"
        R"(# HELP seastar_group_1_metric_2 metric description)" "\n"
        R"(# TYPE seastar_group_1_metric_2 counter)" "\n"
        R"(seastar_group_1_metric_2{label-0="label-0-2",shard="0"} 123)" "\n"
    );
}

SEASTAR_TEST_CASE(test_counter_without_help) {
    test_config cfg{data_type::COUNTER};
    cfg.show_help = false;
    return prometheus_test_fixture::run_metrics_test(cfg, {},
        R"(# TYPE seastar_group_1_metric_0 counter)" "\n"
        R"(seastar_group_1_metric_0{label-0="label-0-0",shard="0"} 123)" "\n"
    );
}

SEASTAR_TEST_CASE(test_counter_with_aggregation) {
    // Create 2 counters with the same metric name but different labels
    // Aggregation combines metrics with the same name, sums their values (123+123=246),
    // and drops the varying labels (keeping only common labels like "shard")
    test_config cfg{data_type::COUNTER, 2};
    cfg.aggregation_mode = aggr_mode::AGGR_LABEL_0;
    cfg.same_metric_name = true;
    return prometheus_test_fixture::run_metrics_test(cfg, {},
        R"(# HELP seastar_group_1_metric metric description)" "\n"
        R"(# TYPE seastar_group_1_metric counter)" "\n"
        R"(seastar_group_1_metric{shard="0"} 246)" "\n"
    );
}

SEASTAR_TEST_CASE(test_label_value_escaping) {
    // Test that label values containing special characters (quotes, backslashes, newlines)
    // are properly escaped according to Prometheus text format specification
    test_config cfg{data_type::COUNTER};
    sm::label label_special{"special"};
    cfg.extra_label = label_special("value with \"quote\", \\backslash and \nline break");
    return prometheus_test_fixture::run_metrics_test(cfg, {},
        R"(# HELP seastar_group_1_metric_0 metric description)" "\n"
        R"(# TYPE seastar_group_1_metric_0 counter)" "\n"
        R"(seastar_group_1_metric_0{label-0="label-0-0",shard="0",special="value with \"quote\", \\backslash and \nline break"} 123)" "\n"
    );
}

SEASTAR_TEST_CASE(test_label_starting_with_double_underscore) {
    // Test that labels starting with __ are filtered out and do not appear in the output
    test_config cfg{data_type::COUNTER};
    sm::label internal_label{"__internal"};
    cfg.extra_label = internal_label("should_not_appear");
    return prometheus_test_fixture::run_metrics_test(cfg, {},
        R"(# HELP seastar_group_1_metric_0 metric description)" "\n"
        R"(# TYPE seastar_group_1_metric_0 counter)" "\n"
        R"(seastar_group_1_metric_0{label-0="label-0-0",shard="0"} 123)" "\n"
    );
}


SEASTAR_TEST_CASE(test_metric_aggregate_by_labels_basic_counter) {
    // Create aggregator that aggregates by "shard" label
    labels_list_type aggregate_labels = {"shard"};
    metric_aggregate_by_labels aggregator(aggregate_labels);

    // Create two counter metrics with same name but different shard labels
    mi::labels_type labels1;
    labels1["name"] = "counter1";
    labels1["shard"] = "0";

    mi::labels_type labels2;
    labels2["name"] = "counter1";
    labels2["shard"] = "1";

    // Add counter values
    mi::metric_value value1(123, mi::data_type::COUNTER);
    mi::metric_value value2(456, mi::data_type::COUNTER);

    aggregator.add(value1, labels1);
    aggregator.add(value2, labels2);

    // After aggregation, should have one metric with "shard" label removed
    const auto& values = aggregator.get_values();
    BOOST_REQUIRE_EQUAL(values.size(), 1);

    // Check that the aggregated value has the correct labels (without "shard")
    auto it = values.begin();
    // Labels are already filtered (aggregated labels removed)
    BOOST_REQUIRE_EQUAL(it->second.labels.size(), 1);
    BOOST_REQUIRE_EQUAL(it->second.labels.at("name").value(), "counter1");

    // Check that values were summed: 123 + 456 = 579
    BOOST_REQUIRE_EQUAL(it->second.m.d(), 579);

    return make_ready_future<>();
}

SEASTAR_TEST_CASE(test_metric_aggregate_by_labels_multiple_labels) {
    // Create aggregator that aggregates by "shard" and "cpu" labels
    labels_list_type aggregate_labels = {"shard", "cpu"};
    metric_aggregate_by_labels aggregator(aggregate_labels);

    // Create metrics with multiple labels
    mi::labels_type labels1;
    labels1["name"] = "metric1";
    labels1["type"] = "typeA";
    labels1["shard"] = "0";
    labels1["cpu"] = "1";

    mi::labels_type labels2;
    labels2["name"] = "metric1";
    labels2["type"] = "typeA";
    labels2["shard"] = "1";
    labels2["cpu"] = "2";

    mi::metric_value value1(100, mi::data_type::COUNTER);
    mi::metric_value value2(200, mi::data_type::COUNTER);

    aggregator.add(value1, labels1);
    aggregator.add(value2, labels2);

    // After aggregation, should have one metric with "shard" and "cpu" removed
    const auto& values = aggregator.get_values();
    BOOST_REQUIRE_EQUAL(values.size(), 1);

    auto it = values.begin();
    // Labels are already filtered (aggregated labels removed)
    BOOST_REQUIRE_EQUAL(it->second.labels.size(), 2);
    BOOST_REQUIRE_EQUAL(it->second.labels.at("name").value(), "metric1");
    BOOST_REQUIRE_EQUAL(it->second.labels.at("type").value(), "typeA");
    BOOST_REQUIRE_EQUAL(it->second.m.d(), 300);

    return make_ready_future<>();
}

SEASTAR_TEST_CASE(test_metric_aggregate_by_labels_different_label_values) {
    // Create aggregator that aggregates by "shard"
    labels_list_type aggregate_labels = {"shard"};
    metric_aggregate_by_labels aggregator(aggregate_labels);

    // Create metrics with different non-aggregated label values
    mi::labels_type labels1;
    labels1["name"] = "metric1";
    labels1["shard"] = "0";

    mi::labels_type labels2;
    labels2["name"] = "metric2";
    labels2["shard"] = "0";

    mi::metric_value value1(100, mi::data_type::COUNTER);
    mi::metric_value value2(200, mi::data_type::COUNTER);

    aggregator.add(value1, labels1);
    aggregator.add(value2, labels2);

    // Should have TWO separate aggregated metrics (different "name" values)
    const auto& values = aggregator.get_values();
    BOOST_REQUIRE_EQUAL(values.size(), 2);

    // Verify we have both metrics by iterating
    int count_metric1 = 0;
    int count_metric2 = 0;
    for (auto it = values.begin(); it != values.end(); ++it) {
        if (it->second.labels.at("name").value() == "metric1") {
            count_metric1++;
            BOOST_REQUIRE_EQUAL(it->second.m.d(), 100);
        } else if (it->second.labels.at("name").value() == "metric2") {
            count_metric2++;
            BOOST_REQUIRE_EQUAL(it->second.m.d(), 200);
        }
    }
    BOOST_REQUIRE_EQUAL(count_metric1, 1);
    BOOST_REQUIRE_EQUAL(count_metric2, 1);

    return make_ready_future<>();
}

SEASTAR_TEST_CASE(test_metric_aggregate_by_labels_empty) {
    // Create aggregator but don't add any metrics
    labels_list_type aggregate_labels = {"shard"};
    metric_aggregate_by_labels aggregator(aggregate_labels);

    BOOST_REQUIRE(aggregator.empty());
    BOOST_REQUIRE_EQUAL(aggregator.get_values().size(), 0);

    return make_ready_future<>();
}

SEASTAR_TEST_CASE(test_metric_aggregate_by_labels_no_aggregation) {
    // Create aggregator with empty list (no labels to aggregate)
    labels_list_type aggregate_labels = {};
    metric_aggregate_by_labels aggregator(aggregate_labels);

    mi::labels_type labels1;
    labels1["name"] = "metric1";
    labels1["shard"] = "0";

    mi::labels_type labels2;
    labels2["name"] = "metric1";
    labels2["shard"] = "1";

    mi::metric_value value1(100, mi::data_type::COUNTER);
    mi::metric_value value2(200, mi::data_type::COUNTER);

    aggregator.add(value1, labels1);
    aggregator.add(value2, labels2);

    // Should have TWO separate metrics (no aggregation)
    const auto& values = aggregator.get_values();
    BOOST_REQUIRE_EQUAL(values.size(), 2);

    return make_ready_future<>();
}

SEASTAR_TEST_CASE(test_metric_aggregate_by_labels_real_counters) {
    // Test with real (floating point) counters
    labels_list_type aggregate_labels = {"shard"};
    metric_aggregate_by_labels aggregator(aggregate_labels);

    mi::labels_type labels1;
    labels1["name"] = "counter";
    labels1["shard"] = "0";

    mi::labels_type labels2;
    labels2["name"] = "counter";
    labels2["shard"] = "1";

    mi::metric_value value1(123.5, mi::data_type::REAL_COUNTER);
    mi::metric_value value2(456.7, mi::data_type::REAL_COUNTER);

    aggregator.add(value1, labels1);
    aggregator.add(value2, labels2);

    const auto& values = aggregator.get_values();
    BOOST_REQUIRE_EQUAL(values.size(), 1);

    auto it = values.begin();
    BOOST_REQUIRE_CLOSE(it->second.m.d(), 580.2, 0.001);

    return make_ready_future<>();
}

SEASTAR_TEST_CASE(test_metric_aggregate_by_labels_histograms) {
    // Test with histograms
    labels_list_type aggregate_labels = {"shard"};
    metric_aggregate_by_labels aggregator(aggregate_labels);

    mi::labels_type labels1;
    labels1["name"] = "histogram";
    labels1["shard"] = "0";

    mi::labels_type labels2;
    labels2["name"] = "histogram";
    labels2["shard"] = "1";

    // Create simple histograms
    sm::histogram h1;
    h1.sample_count = 10;
    h1.sample_sum = 100;
    h1.buckets.push_back(sm::histogram_bucket{5, 1.0});
    h1.buckets.push_back(sm::histogram_bucket{10, 2.0});

    sm::histogram h2;
    h2.sample_count = 20;
    h2.sample_sum = 300;
    h2.buckets.push_back(sm::histogram_bucket{8, 1.0});
    h2.buckets.push_back(sm::histogram_bucket{20, 2.0});

    mi::metric_value value1(h1);
    mi::metric_value value2(h2);

    aggregator.add(value1, labels1);
    aggregator.add(value2, labels2);

    const auto& values = aggregator.get_values();
    BOOST_REQUIRE_EQUAL(values.size(), 1);

    auto it = values.begin();
    const auto& aggregated_hist = it->second.m.get_histogram();

    // Check aggregated histogram values
    BOOST_REQUIRE_EQUAL(aggregated_hist.sample_count, 30);
    BOOST_REQUIRE_EQUAL(aggregated_hist.sample_sum, 400);
    BOOST_REQUIRE_EQUAL(aggregated_hist.buckets.size(), 2);
    BOOST_REQUIRE_EQUAL(aggregated_hist.buckets[0].count, 13);
    BOOST_REQUIRE_EQUAL(aggregated_hist.buckets[1].count, 30);

    return make_ready_future<>();
}

SEASTAR_TEST_CASE(test_metric_aggregate_by_labels_same_metric_added_twice) {
    // Test adding the same metric configuration twice (should aggregate)
    labels_list_type aggregate_labels = {"shard"};
    metric_aggregate_by_labels aggregator(aggregate_labels);

    mi::labels_type labels;
    labels["name"] = "metric";
    labels["shard"] = "0";

    mi::metric_value value1(100, mi::data_type::COUNTER);
    mi::metric_value value2(150, mi::data_type::COUNTER);

    aggregator.add(value1, labels);
    aggregator.add(value2, labels);

    const auto& values = aggregator.get_values();
    BOOST_REQUIRE_EQUAL(values.size(), 1);

    auto it = values.begin();
    BOOST_REQUIRE_EQUAL(it->second.m.d(), 250);

    return make_ready_future<>();
}

// Tests for family_filter functionality
// These tests use make_family_filter to exercise the same filtering logic used by the HTTP handler

using name_filter = sp::details::name_filter;

SEASTAR_TEST_CASE(test_family_filter_exact_match) {
    // Filter to match only metric_1 by exact name
    test_config cfg{data_type::COUNTER, 3};
    cfg.family_filter = sp::details::make_family_filter({
        name_filter{"group_1_metric_1", false}
    });
    return prometheus_test_fixture::run_metrics_test(cfg, {},
        R"(# HELP seastar_group_1_metric_1 metric description)" "\n"
        R"(# TYPE seastar_group_1_metric_1 counter)" "\n"
        R"(seastar_group_1_metric_1{label-0="label-0-1",shard="0"} 123)" "\n"
    );
}

SEASTAR_TEST_CASE(test_family_filter_prefix_match) {
    // Filter to match metrics starting with "group_1_metric_" (prefix match)
    // This should match all 3 metrics
    test_config cfg{data_type::COUNTER, 3};
    cfg.family_filter = sp::details::make_family_filter({
        name_filter{"group_1_metric_", true}
    });
    return prometheus_test_fixture::run_metrics_test(cfg, {},
        R"(# HELP seastar_group_1_metric_0 metric description)" "\n"
        R"(# TYPE seastar_group_1_metric_0 counter)" "\n"
        R"(seastar_group_1_metric_0{label-0="label-0-0",shard="0"} 123)" "\n"
        R"(# HELP seastar_group_1_metric_1 metric description)" "\n"
        R"(# TYPE seastar_group_1_metric_1 counter)" "\n"
        R"(seastar_group_1_metric_1{label-0="label-0-1",shard="0"} 123)" "\n"
        R"(# HELP seastar_group_1_metric_2 metric description)" "\n"
        R"(# TYPE seastar_group_1_metric_2 counter)" "\n"
        R"(seastar_group_1_metric_2{label-0="label-0-2",shard="0"} 123)" "\n"
    );
}

SEASTAR_TEST_CASE(test_family_filter_multiple_exact_matches) {
    // Filter to match metric_0 and metric_2 by exact name (not metric_1)
    test_config cfg{data_type::COUNTER, 3};
    cfg.family_filter = sp::details::make_family_filter({
        name_filter{"group_1_metric_0", false},
        name_filter{"group_1_metric_2", false}
    });
    return prometheus_test_fixture::run_metrics_test(cfg, {},
        R"(# HELP seastar_group_1_metric_0 metric description)" "\n"
        R"(# TYPE seastar_group_1_metric_0 counter)" "\n"
        R"(seastar_group_1_metric_0{label-0="label-0-0",shard="0"} 123)" "\n"
        R"(# HELP seastar_group_1_metric_2 metric description)" "\n"
        R"(# TYPE seastar_group_1_metric_2 counter)" "\n"
        R"(seastar_group_1_metric_2{label-0="label-0-2",shard="0"} 123)" "\n"
    );
}

SEASTAR_TEST_CASE(test_family_filter_combined_exact_and_prefix) {
    // Filter that matches metric_0 exactly OR any metric starting with "group_1_metric_2"
    test_config cfg{data_type::COUNTER, 3};
    cfg.family_filter = sp::details::make_family_filter({
        name_filter{"group_1_metric_0", false},
        name_filter{"group_1_metric_2", true}
    });
    return prometheus_test_fixture::run_metrics_test(cfg, {},
        R"(# HELP seastar_group_1_metric_0 metric description)" "\n"
        R"(# TYPE seastar_group_1_metric_0 counter)" "\n"
        R"(seastar_group_1_metric_0{label-0="label-0-0",shard="0"} 123)" "\n"
        R"(# HELP seastar_group_1_metric_2 metric description)" "\n"
        R"(# TYPE seastar_group_1_metric_2 counter)" "\n"
        R"(seastar_group_1_metric_2{label-0="label-0-2",shard="0"} 123)" "\n"
    );
}

SEASTAR_TEST_CASE(test_family_filter_empty_matches_all) {
    // Empty filter list matches all metrics
    test_config cfg{data_type::COUNTER, 3};
    cfg.family_filter = sp::details::make_family_filter({});
    return prometheus_test_fixture::run_metrics_test(cfg, {},
        R"(# HELP seastar_group_1_metric_0 metric description)" "\n"
        R"(# TYPE seastar_group_1_metric_0 counter)" "\n"
        R"(seastar_group_1_metric_0{label-0="label-0-0",shard="0"} 123)" "\n"
        R"(# HELP seastar_group_1_metric_1 metric description)" "\n"
        R"(# TYPE seastar_group_1_metric_1 counter)" "\n"
        R"(seastar_group_1_metric_1{label-0="label-0-1",shard="0"} 123)" "\n"
        R"(# HELP seastar_group_1_metric_2 metric description)" "\n"
        R"(# TYPE seastar_group_1_metric_2 counter)" "\n"
        R"(seastar_group_1_metric_2{label-0="label-0-2",shard="0"} 123)" "\n"
    );
}

SEASTAR_TEST_CASE(test_family_filter_no_match) {
    // Filter with non-existent metric name - should produce empty output
    test_config cfg{data_type::COUNTER, 3};
    cfg.family_filter = sp::details::make_family_filter({
        name_filter{"nonexistent_metric", false}
    });
    return prometheus_test_fixture::run_metrics_test(cfg, {}, "");
}

SEASTAR_TEST_CASE(test_family_filter_with_label_filter) {
    // Combine family filter with label filter
    // Family filter: only metric_0 and metric_1
    // Label filter: exclude label-0-0
    // Result: only metric_1 (metric_0 excluded by label filter)
    sp::details::filter_t label_filter = [](const sm::impl::labels_type& labels) {
        auto it = labels.find("label-0");
        return it == labels.end() || it->second.value() != "label-0-0";
    };

    test_config cfg{data_type::COUNTER, 3};
    cfg.family_filter = sp::details::make_family_filter({
        name_filter{"group_1_metric_0", false},
        name_filter{"group_1_metric_1", false}
    });
    cfg.filter = label_filter;
    return prometheus_test_fixture::run_metrics_test(cfg, {},
        R"(# HELP seastar_group_1_metric_1 metric description)" "\n"
        R"(# TYPE seastar_group_1_metric_1 counter)" "\n"
        R"(seastar_group_1_metric_1{label-0="label-0-1",shard="0"} 123)" "\n"
    );
}

SEASTAR_TEST_CASE(test_family_filter_exact_match_with_prefix) {
    // Filter using the full prefixed name "seastar_group_1_metric_0"
    // with prefix "seastar" should strip to "group_1_metric_0" and match
    test_config cfg{data_type::COUNTER, 3};
    cfg.family_filter = sp::details::make_family_filter({
        name_filter{"seastar_group_1_metric_0", false}
    }, "seastar");
    return prometheus_test_fixture::run_metrics_test(cfg, {},
        R"(# HELP seastar_group_1_metric_0 metric description)" "\n"
        R"(# TYPE seastar_group_1_metric_0 counter)" "\n"
        R"(seastar_group_1_metric_0{label-0="label-0-0",shard="0"} 123)" "\n"
    );
}

SEASTAR_TEST_CASE(test_family_filter_prefix_match_with_prefix) {
    // Filter using "seastar_group_1_metric_" as prefix filter
    // with prefix "seastar" should strip to "group_1_metric_" and match all group_1_metric_* metrics
    test_config cfg{data_type::COUNTER, 3};
    cfg.family_filter = sp::details::make_family_filter({
        name_filter{"seastar_group_1_metric_", true}
    }, "seastar");
    return prometheus_test_fixture::run_metrics_test(cfg, {},
        R"(# HELP seastar_group_1_metric_0 metric description)" "\n"
        R"(# TYPE seastar_group_1_metric_0 counter)" "\n"
        R"(seastar_group_1_metric_0{label-0="label-0-0",shard="0"} 123)" "\n"
        R"(# HELP seastar_group_1_metric_1 metric description)" "\n"
        R"(# TYPE seastar_group_1_metric_1 counter)" "\n"
        R"(seastar_group_1_metric_1{label-0="label-0-1",shard="0"} 123)" "\n"
        R"(# HELP seastar_group_1_metric_2 metric description)" "\n"
        R"(# TYPE seastar_group_1_metric_2 counter)" "\n"
        R"(seastar_group_1_metric_2{label-0="label-0-2",shard="0"} 123)" "\n"
    );
}

SEASTAR_TEST_CASE(test_family_filter_mixed_prefixed_and_unprefixed) {
    // Filter with both prefixed ("seastar_group_1_metric_0") and unprefixed ("group_1_metric_2") names
    // Both should match their respective metrics
    test_config cfg{data_type::COUNTER, 3};
    cfg.family_filter = sp::details::make_family_filter({
        name_filter{"seastar_group_1_metric_0", false},
        name_filter{"group_1_metric_2", false}
    }, "seastar");
    return prometheus_test_fixture::run_metrics_test(cfg, {},
        R"(# HELP seastar_group_1_metric_0 metric description)" "\n"
        R"(# TYPE seastar_group_1_metric_0 counter)" "\n"
        R"(seastar_group_1_metric_0{label-0="label-0-0",shard="0"} 123)" "\n"
        R"(# HELP seastar_group_1_metric_2 metric description)" "\n"
        R"(# TYPE seastar_group_1_metric_2 counter)" "\n"
        R"(seastar_group_1_metric_2{label-0="label-0-2",shard="0"} 123)" "\n"
    );
}


// Tests of the text representation template cache

static seastar::future<> reset_metrics() {
    co_await smp::invoke_on_all([] {
        remove_existing_metrics();
        prometheus_test_fixture::clear_cache();
    });
}

SEASTAR_TEST_CASE(test_cache_values_change) {
    co_await reset_metrics();
    double value = 1;
    sm::metric_groups metrics;
    metrics.add_group("cache", {
        sm::make_gauge("gauge", sm::description("gauge"), [&value] { return value; }),
    });

    auto expected = [] (std::string_view v) {
        return fmt::format("# TYPE seastar_cache_gauge gauge\nseastar_cache_gauge{{shard=\"0\"}} {}\n", v);
    };
    BOOST_REQUIRE_EQUAL(co_await prometheus_test_fixture::scrape({}, all_metrics()), expected("1.000000"));
    value = -12345.5;
    BOOST_REQUIRE_EQUAL(co_await prometheus_test_fixture::scrape({}, all_metrics()), expected("-12345.500000"));
    value = std::numeric_limits<double>::quiet_NaN();
    BOOST_REQUIRE_EQUAL(co_await prometheus_test_fixture::scrape({}, all_metrics()), expected("nan"));

    auto stats = prometheus_test_fixture::cache_stats();
    BOOST_REQUIRE_EQUAL(stats.builds, 1);
    BOOST_REQUIRE_EQUAL(stats.hits, 2);
}

SEASTAR_TEST_CASE(test_cache_counter_not_representable) {
    co_await reset_metrics();
    uint64_t value = 7;
    sm::metric_groups metrics;
    metrics.add_group("cache", {
        sm::make_counter("counter", sm::description("counter"), [&value] { return value; }),
    });

    auto expected = [] (std::string_view v) {
        return fmt::format("# TYPE seastar_cache_counter counter\nseastar_cache_counter{{shard=\"0\"}} {}\n", v);
    };
    BOOST_REQUIRE_EQUAL(co_await prometheus_test_fixture::scrape({}, all_metrics()), expected("7"));
    value = std::numeric_limits<uint64_t>::max();
    BOOST_REQUIRE_EQUAL(co_await prometheus_test_fixture::scrape({}, all_metrics()), expected("NaN"));
    value = 8;
    BOOST_REQUIRE_EQUAL(co_await prometheus_test_fixture::scrape({}, all_metrics()), expected("8"));
    BOOST_REQUIRE_EQUAL(prometheus_test_fixture::cache_stats().builds, 1);
}

SEASTAR_TEST_CASE(test_cache_invalidated_by_shape_change) {
    co_await reset_metrics();
    sm::metric_groups metrics;
    metrics.add_group("cache", {
        sm::make_gauge("a", sm::description("a"), [] { return 1; }),
    });
    auto a = "# TYPE seastar_cache_a gauge\nseastar_cache_a{shard=\"0\"} 1.000000\n"s;
    auto b = "# TYPE seastar_cache_b gauge\nseastar_cache_b{shard=\"0\"} 2.000000\n"s;
    BOOST_REQUIRE_EQUAL(co_await prometheus_test_fixture::scrape({}, all_metrics()), a);

    {
        sm::metric_groups more;
        more.add_group("cache", {
            sm::make_gauge("b", sm::description("b"), [] { return 2; }),
        });
        BOOST_REQUIRE_EQUAL(co_await prometheus_test_fixture::scrape({}, all_metrics()), a + b);
        BOOST_REQUIRE_EQUAL(prometheus_test_fixture::cache_stats().builds, 2);
    }
    BOOST_REQUIRE_EQUAL(co_await prometheus_test_fixture::scrape({}, all_metrics()), a);
    BOOST_REQUIRE_EQUAL(prometheus_test_fixture::cache_stats().builds, 3);

    // A metric added on another shard changes the shape too
    auto other = (this_shard_id() + 1) % this_smp_shard_count();
    if (other != this_shard_id()) {
        co_await smp::submit_to(other, [] {
            static thread_local std::optional<sm::metric_groups> more;
            more.emplace();
            more->add_group("cache", {
                sm::make_gauge("c", sm::description("c"), [] { return 3; }),
            });
        });
        auto out = co_await prometheus_test_fixture::scrape({}, all_metrics());
        BOOST_REQUIRE_NE(out.find("seastar_cache_c"), sstring::npos);
        BOOST_REQUIRE_EQUAL(prometheus_test_fixture::cache_stats().builds, 4);
        co_await smp::submit_to(other, [] {
            remove_existing_metrics();
        });
    }

    // Different keys have different templates. Templates of older shapes
    // are kept until they expire, so only the new one is counted.
    auto entries = prometheus_test_fixture::cache_stats().entries;
    BOOST_REQUIRE_EQUAL(co_await prometheus_test_fixture::scrape({}, all_metrics(sp::details::filter_key{.names = {{"other", true}}})), a);
    BOOST_REQUIRE_EQUAL(prometheus_test_fixture::cache_stats().entries, entries + 1);
}

SEASTAR_TEST_CASE(test_cache_skip_when_empty) {
    co_await reset_metrics();
    uint64_t value = 0;
    sm::metric_groups metrics;
    metrics.add_group("cache", {
        sm::make_counter("counter", [&value] { return value; }, sm::description("counter"))
            .set_skip_when_empty(true),
        sm::make_counter("counter", [&value] { return value + 1; }, sm::description("counter"), {sm::label("l")("x")}),
    });

    auto only_second = [] (uint64_t v) {
        return fmt::format("# TYPE seastar_cache_counter counter\n"
                           "seastar_cache_counter{{l=\"x\",shard=\"0\"}} {}\n", v);
    };
    auto both = [] (uint64_t v) {
        return fmt::format("# TYPE seastar_cache_counter counter\n"
                           "seastar_cache_counter{{l=\"x\",shard=\"0\"}} {}\n"
                           "seastar_cache_counter{{shard=\"0\"}} {}\n", v + 1, v);
    };
    // Metrics which were never used aren't reported
    BOOST_REQUIRE_EQUAL(co_await prometheus_test_fixture::scrape({}, all_metrics()), only_second(1));
    BOOST_REQUIRE_EQUAL(co_await prometheus_test_fixture::scrape({}, all_metrics()), only_second(1));
    // Using a metric changes the shape
    value = 5;
    BOOST_REQUIRE_EQUAL(co_await prometheus_test_fixture::scrape({}, all_metrics()), both(5));
    // From then on, it's reported even when empty
    value = 0;
    BOOST_REQUIRE_EQUAL(co_await prometheus_test_fixture::scrape({}, all_metrics()), both(0));
    auto stats = prometheus_test_fixture::cache_stats();
    BOOST_REQUIRE_EQUAL(stats.builds, 2);
    BOOST_REQUIRE_EQUAL(stats.hits, 2);
}

SEASTAR_TEST_CASE(test_cache_skip_when_empty_family) {
    co_await reset_metrics();
    uint64_t value = 0;
    sm::metric_groups metrics;
    metrics.add_group("cache", {
        sm::make_counter("counter", [&value] { return value; }, sm::description("counter"))
            .set_skip_when_empty(true),
    });

    // A family whose metrics were never used isn't reported
    BOOST_REQUIRE_EQUAL(co_await prometheus_test_fixture::scrape({}, all_metrics()), "");
    value = 5;
    BOOST_REQUIRE_EQUAL(co_await prometheus_test_fixture::scrape({}, all_metrics()),
        "# TYPE seastar_cache_counter counter\nseastar_cache_counter{shard=\"0\"} 5\n");
    value = 0;
    BOOST_REQUIRE_EQUAL(co_await prometheus_test_fixture::scrape({}, all_metrics()),
        "# TYPE seastar_cache_counter counter\nseastar_cache_counter{shard=\"0\"} 0\n");
    BOOST_REQUIRE_EQUAL(prometheus_test_fixture::cache_stats().builds, 2);
}

SEASTAR_TEST_CASE(test_cache_summary_sum_and_count) {
    co_await reset_metrics();
    sm::histogram h;
    h.buckets = {{3, 0.5}};
    sm::metric_groups metrics;
    metrics.add_group("cache", {
        make_summary("summary", sm::description("summary"), [&h] { return h; }),
    });
    auto quantile = "seastar_cache_summary{quantile=\"0.500000\",shard=\"0\"} 3\n"s;
    auto header = "# TYPE seastar_cache_summary summary\n"s;
    // A zero sum and count aren't reported
    BOOST_REQUIRE_EQUAL(co_await prometheus_test_fixture::scrape({}, all_metrics()), header + quantile);
    BOOST_REQUIRE_EQUAL(co_await prometheus_test_fixture::scrape({}, all_metrics()), header + quantile);
    BOOST_REQUIRE_EQUAL(prometheus_test_fixture::cache_stats().invalidations, 0);
    // Non-zero ones invalidate the template, and are then reported even if zero
    h.sample_sum = 2;
    h.sample_count = 1;
    BOOST_REQUIRE_EQUAL(co_await prometheus_test_fixture::scrape({}, all_metrics()),
        header + "seastar_cache_summary_sum{shard=\"0\"} 2\nseastar_cache_summary_count{shard=\"0\"} 1\n" + quantile);
    h.sample_sum = 0;
    h.sample_count = 0;
    BOOST_REQUIRE_EQUAL(co_await prometheus_test_fixture::scrape({}, all_metrics()),
        header + "seastar_cache_summary_sum{shard=\"0\"} 0\nseastar_cache_summary_count{shard=\"0\"} 0\n" + quantile);
    auto stats = prometheus_test_fixture::cache_stats();
    BOOST_REQUIRE_EQUAL(stats.invalidations, 1);
    BOOST_REQUIRE_EQUAL(stats.builds, 2);
}

SEASTAR_TEST_CASE(test_cache_histogram_buckets_change) {
    co_await reset_metrics();
    sm::histogram h;
    h.sample_count = 2;
    h.sample_sum = 3;
    h.buckets = {{1, 1.0}, {2, 2.0}};
    sm::metric_groups metrics;
    metrics.add_group("cache", {
        sm::make_histogram("histogram", sm::description("histogram"), [&h] { return h; }),
    });

    auto expected = [&h] {
        auto s = fmt::format("# TYPE seastar_cache_histogram histogram\n"
                             "seastar_cache_histogram_sum{{shard=\"0\"}} {:g}\n"
                             "seastar_cache_histogram_count{{shard=\"0\"}} {}\n", h.sample_sum, h.sample_count);
        for (auto& b : h.buckets) {
            s += fmt::format("seastar_cache_histogram_bucket{{le=\"{:f}\",shard=\"0\"}} {}\n", b.upper_bound, b.count);
        }
        s += fmt::format("seastar_cache_histogram_bucket{{le=\"+Inf\",shard=\"0\"}} {}\n", h.sample_count);
        return s;
    };
    BOOST_REQUIRE_EQUAL(co_await prometheus_test_fixture::scrape({}, all_metrics()), expected());
    h.buckets[1].count = 4;
    h.sample_count = 4;
    BOOST_REQUIRE_EQUAL(co_await prometheus_test_fixture::scrape({}, all_metrics()), expected());
    BOOST_REQUIRE_EQUAL(prometheus_test_fixture::cache_stats().invalidations, 0);

    // Different upper bounds invalidate the template
    h.buckets[1].upper_bound = 3;
    BOOST_REQUIRE_EQUAL(co_await prometheus_test_fixture::scrape({}, all_metrics()), expected());
    // As does a different number of buckets
    h.buckets.push_back({5, 10});
    h.sample_count = 5;
    BOOST_REQUIRE_EQUAL(co_await prometheus_test_fixture::scrape({}, all_metrics()), expected());
    auto stats = prometheus_test_fixture::cache_stats();
    BOOST_REQUIRE_EQUAL(stats.invalidations, 2);
    BOOST_REQUIRE_EQUAL(stats.builds, 3);
    BOOST_REQUIRE_EQUAL(stats.hits, 1);
}

SEASTAR_TEST_CASE(test_cache_expiry) {
    co_await reset_metrics();
    sm::metric_groups metrics;
    metrics.add_group("cache", {
        sm::make_gauge("gauge", sm::description("gauge"), [] { return 1; }),
    });
    prometheus_test_fixture::set_cache_ttl(std::chrono::milliseconds(100));
    co_await prometheus_test_fixture::scrape({}, all_metrics());
    // Using the template keeps it from expiring
    for (int i = 0; i < 10; ++i) {
        co_await seastar::sleep(std::chrono::milliseconds(30));
        co_await prometheus_test_fixture::scrape({}, all_metrics());
    }
    BOOST_REQUIRE_EQUAL(prometheus_test_fixture::cache_stats().builds, 1);
    // Unused, it expires
    co_await seastar::sleep(std::chrono::milliseconds(300));
    co_await prometheus_test_fixture::scrape({}, all_metrics());
    prometheus_test_fixture::set_cache_ttl(std::chrono::minutes(5));
    auto stats = prometheus_test_fixture::cache_stats();
    BOOST_REQUIRE_EQUAL(stats.builds, 2);
    BOOST_REQUIRE_EQUAL(stats.hits, 10);
    BOOST_REQUIRE_EQUAL(stats.entries, 1);
}

SEASTAR_TEST_CASE(test_cache_aggregation) {
    co_await reset_metrics();
    uint64_t v1 = 1, v2 = 0;
    sm::metric_groups metrics;
    sm::label l("l");
    metrics.add_group("cache", {
        sm::make_counter("counter", [&v1] { return v1; }, sm::description("counter"), {l("a")}).aggregate({l}),
        sm::make_counter("counter", [&v2] { return v2; }, sm::description("counter"), {l("b")})
            .aggregate({l}).set_skip_when_empty(true),
    });
    auto expected = [] (uint64_t v) {
        return fmt::format("# TYPE seastar_cache_counter counter\nseastar_cache_counter{{shard=\"0\"}} {}\n", v);
    };
    BOOST_REQUIRE_EQUAL(co_await prometheus_test_fixture::scrape({}, all_metrics()), expected(1));
    v2 = 10;
    BOOST_REQUIRE_EQUAL(co_await prometheus_test_fixture::scrape({}, all_metrics()), expected(11));
    v2 = 0;
    BOOST_REQUIRE_EQUAL(co_await prometheus_test_fixture::scrape({}, all_metrics()), expected(1));
    BOOST_REQUIRE_EQUAL(prometheus_test_fixture::cache_stats().builds, 2);
}

SEASTAR_TEST_CASE(test_cache_shared_between_shards) {
    co_await reset_metrics();
    double value = 1;
    sm::metric_groups metrics;
    metrics.add_group("cache", {
        sm::make_gauge("gauge", sm::description("gauge"), [&value] { return value; }),
    });
    auto expected = co_await prometheus_test_fixture::scrape({}, all_metrics());
    BOOST_REQUIRE_EQUAL(prometheus_test_fixture::cache_stats().builds, 1);

    // All other shards got the template, and use it
    co_await smp::invoke_on_others([expected] () -> future<> {
        BOOST_REQUIRE_EQUAL(prometheus_test_fixture::cache_stats().entries, 1);
        BOOST_REQUIRE_EQUAL(co_await prometheus_test_fixture::scrape({}, all_metrics()), expected);
        auto stats = prometheus_test_fixture::cache_stats();
        BOOST_REQUIRE_EQUAL(stats.builds, 0);
        BOOST_REQUIRE_EQUAL(stats.hits, 1);
    });

    // A template built by another shard is shared with this one
    co_await reset_metrics();
    sm::metric_groups more;
    more.add_group("cache", {
        sm::make_gauge("gauge", sm::description("gauge"), [&value] { return value; }),
    });
    auto other = (this_shard_id() + 1) % this_smp_shard_count();
    expected = co_await smp::submit_to(other, [] {
        return prometheus_test_fixture::scrape({}, all_metrics());
    });
    BOOST_REQUIRE_EQUAL(co_await prometheus_test_fixture::scrape({}, all_metrics()), expected);
    auto stats = prometheus_test_fixture::cache_stats();
    BOOST_REQUIRE_EQUAL(stats.builds, other == this_shard_id() ? 1 : 0);
}

SEASTAR_TEST_CASE(test_cache_shared_per_numa_node) {
    co_await reset_metrics();
    sm::metric_groups metrics;
    metrics.add_group("cache", {
        sm::make_gauge("gauge", sm::description("gauge"), [] { return 1; }),
    });
    // Pretend that shards alternate between two NUMA nodes
    std::vector<unsigned> mapping;
    for (unsigned shard = 0; shard < this_smp_shard_count(); ++shard) {
        mapping.push_back(shard % 2);
    }
    prometheus_test_fixture::set_numa_node_mapping(mapping);
    auto expected = co_await prometheus_test_fixture::scrape({}, all_metrics());
    prometheus_test_fixture::set_numa_node_mapping(std::nullopt);

    std::vector<const void*> templates;
    for (unsigned shard = 0; shard < this_smp_shard_count(); ++shard) {
        templates.push_back(co_await smp::submit_to(shard, [] () -> future<const void*> {
            BOOST_REQUIRE_EQUAL(prometheus_test_fixture::cache_stats().entries, 1);
            auto t = prometheus_test_fixture::cached_template();
            // Each shard's copy works
            co_await prometheus_test_fixture::scrape({}, all_metrics());
            BOOST_REQUIRE_EQUAL(prometheus_test_fixture::cache_stats().builds, this_shard_id() == 0 ? 1 : 0);
            co_return t;
        }));
    }
    for (unsigned shard = 0; shard < this_smp_shard_count(); ++shard) {
        // Shards on the same node share one copy, and each node has its own
        BOOST_REQUIRE_EQUAL(templates[shard], templates[shard % 2]);
        if (shard % 2) {
            BOOST_REQUIRE_NE(templates[shard], templates[0]);
        }
    }
}
