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
    // the text format to use
    sp::text_format format = sp::text_format::prometheus_0_0_4;
    // overrides the generated metric name
    std::optional<std::string> metric_name;
    // if true, strip the native histogram information from histograms
    bool classic_histogram = false;
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
    // Serialize the currently registered metrics with the given configuration and return the output
    static seastar::future<std::string> write_metrics(prometheus::config config, sp::details::write_body_args args) {
        std::stringstream ss;
        output_stream<char> out{data_sink{std::make_unique<testing::memory_data_sink_impl>(ss, 10)}};
        co_await prometheus::details::test_access{}.write_body(std::move(config), std::move(args), std::move(out));
        co_return ss.str();
    }

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
            auto metric_name = test_conf.metric_name.value_or(test_conf.same_metric_name ? "metric" : fmt::format("metric_{}", i));
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
                            [histogram = make_historgam(), classic = test_conf.classic_histogram]() {
                                auto h = histogram->to_metrics_histogram();
                                if (classic) {
                                    h.native_histogram.reset();
                                }
                                return h;
                            });
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

        using access = prometheus::details::test_access;

        std::stringstream ss;
        output_stream<char> out{data_sink{std::make_unique<testing::memory_data_sink_impl>(ss, 10)}};
        auto filter = test_conf.filter.value_or(always_true);
        auto family_filter = test_conf.family_filter.value_or([](std::string_view) { return true; });
        co_await access{}.write_body(config,
            sp::details::write_body_args{
                .filter = filter,
                .family_filter = family_filter,
                .use_protobuf_format = false,
                .show_help = test_conf.show_help,
                .enable_aggregation = test_conf.aggregation_mode != aggr_mode::NO_AGGR,
                .format = test_conf.format,
            },
            std::move(out));

        BOOST_REQUIRE_MESSAGE(expected == ss.str(),
            fmt::format("actual output doesn't match expected\nexpected output:\n{}\nactual output:\n{}",
            expected, ss.str()));
    }
};

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


// OpenMetrics 2.0 tests
//
// Metric and label names are reported as is, OpenMetrics 2.0 only recommends type and unit suffixes.

SEASTAR_TEST_CASE(test_om_basic_counter) {
    test_config cfg{data_type::COUNTER};
    cfg.format = sp::text_format::openmetrics_2_0;
    return prometheus_test_fixture::run_metrics_test(cfg, {},
        R"(# TYPE seastar_group_1_metric_0 counter)" "\n"
        R"(# HELP seastar_group_1_metric_0 metric description)" "\n"
        R"(seastar_group_1_metric_0{"label-0"="label-0-0",shard="0"} 123)" "\n"
        R"(# EOF)" "\n"
    );
}

SEASTAR_TEST_CASE(test_om_counter_without_help) {
    test_config cfg{data_type::COUNTER};
    cfg.format = sp::text_format::openmetrics_2_0;
    cfg.show_help = false;
    return prometheus_test_fixture::run_metrics_test(cfg, {},
        R"(# TYPE seastar_group_1_metric_0 counter)" "\n"
        R"(seastar_group_1_metric_0{"label-0"="label-0-0",shard="0"} 123)" "\n"
        R"(# EOF)" "\n"
    );
}

SEASTAR_TEST_CASE(test_om_basic_gauge) {
    test_config cfg{data_type::GAUGE};
    cfg.format = sp::text_format::openmetrics_2_0;
    return prometheus_test_fixture::run_metrics_test(cfg, {},
        R"(# TYPE seastar_group_1_metric_0 gauge)" "\n"
        R"(# HELP seastar_group_1_metric_0 metric description)" "\n"
        R"(seastar_group_1_metric_0{"label-0"="label-0-0",shard="0"} 123.400000)" "\n"
        R"(# EOF)" "\n"
    );
}

SEASTAR_TEST_CASE(test_om_counter_with_global_label) {
    test_config cfg{data_type::COUNTER};
    cfg.format = sp::text_format::openmetrics_2_0;
    prometheus::config prom_cfg;
    sm::label extra_label{"env"};
    prom_cfg.label = extra_label("production");
    return prometheus_test_fixture::run_metrics_test(cfg, prom_cfg,
        R"(# TYPE seastar_group_1_metric_0 counter)" "\n"
        R"(# HELP seastar_group_1_metric_0 metric description)" "\n"
        R"(seastar_group_1_metric_0{env="production","label-0"="label-0-0",shard="0"} 123)" "\n"
        R"(# EOF)" "\n"
    );
}

SEASTAR_TEST_CASE(test_om_counter_with_aggregation) {
    test_config cfg{data_type::COUNTER, 2};
    cfg.format = sp::text_format::openmetrics_2_0;
    cfg.aggregation_mode = aggr_mode::AGGR_LABEL_0;
    cfg.same_metric_name = true;
    return prometheus_test_fixture::run_metrics_test(cfg, {},
        R"(# TYPE seastar_group_1_metric counter)" "\n"
        R"(# HELP seastar_group_1_metric metric description)" "\n"
        R"(seastar_group_1_metric{shard="0"} 246)" "\n"
        R"(# EOF)" "\n"
    );
}

SEASTAR_TEST_CASE(test_om_label_value_escaping) {
    test_config cfg{data_type::COUNTER};
    cfg.format = sp::text_format::openmetrics_2_0;
    sm::label label_special{"special"};
    cfg.extra_label = label_special("value with \"quote\", \\backslash and \nline break");
    return prometheus_test_fixture::run_metrics_test(cfg, {},
        R"(# TYPE seastar_group_1_metric_0 counter)" "\n"
        R"(# HELP seastar_group_1_metric_0 metric description)" "\n"
        R"(seastar_group_1_metric_0{"label-0"="label-0-0",shard="0",special="value with \"quote\", \\backslash and \nline break"} 123)" "\n"
        R"(# EOF)" "\n"
    );
}

SEASTAR_TEST_CASE(test_om_unit_label) {
    // The __unit label is not reported, instead a UNIT line is added. The name is not changed.
    test_config cfg{data_type::COUNTER};
    cfg.format = sp::text_format::openmetrics_2_0;
    sm::label unit_label{"__unit"};
    cfg.extra_label = unit_label("bytes");
    return prometheus_test_fixture::run_metrics_test(cfg, {},
        R"(# TYPE seastar_group_1_metric_0 counter)" "\n"
        R"(# UNIT seastar_group_1_metric_0 bytes)" "\n"
        R"(# HELP seastar_group_1_metric_0 metric description)" "\n"
        R"(seastar_group_1_metric_0{"label-0"="label-0-0",shard="0"} 123)" "\n"
        R"(# EOF)" "\n"
    );
}

SEASTAR_TEST_CASE(test_om_counter_with_total_suffix) {
    // A counter name ending with _total is reported as is
    test_config cfg{data_type::COUNTER};
    cfg.format = sp::text_format::openmetrics_2_0;
    cfg.metric_name = "requests_total";
    return prometheus_test_fixture::run_metrics_test(cfg, {},
        R"(# TYPE seastar_group_1_requests_total counter)" "\n"
        R"(# HELP seastar_group_1_requests_total metric description)" "\n"
        R"(seastar_group_1_requests_total{"label-0"="label-0-0",shard="0"} 123)" "\n"
        R"(# EOF)" "\n"
    );
}

SEASTAR_TEST_CASE(test_om_real_counter) {
    // Counter names are not suffixed with _total, OpenMetrics 2.0 only recommends it
    test_config cfg{data_type::REAL_COUNTER};
    cfg.format = sp::text_format::openmetrics_2_0;
    return prometheus_test_fixture::run_metrics_test(cfg, {},
        R"(# TYPE seastar_group_1_metric_0 counter)" "\n"
        R"(# HELP seastar_group_1_metric_0 metric description)" "\n"
        R"(seastar_group_1_metric_0{"label-0"="label-0-0",shard="0"} 123.400000)" "\n"
        R"(# EOF)" "\n"
    );
}

SEASTAR_TEST_CASE(test_om_gauge_with_total_suffix) {
    // A gauge that happens to end with _total keeps its name
    test_config cfg{data_type::GAUGE};
    cfg.format = sp::text_format::openmetrics_2_0;
    cfg.metric_name = "metric_total";
    return prometheus_test_fixture::run_metrics_test(cfg, {},
        R"(# TYPE seastar_group_1_metric_total gauge)" "\n"
        R"(# HELP seastar_group_1_metric_total metric description)" "\n"
        R"(seastar_group_1_metric_total{"label-0"="label-0-0",shard="0"} 123.400000)" "\n"
        R"(# EOF)" "\n"
    );
}

SEASTAR_TEST_CASE(test_om_unit_label_gauge) {
    test_config cfg{data_type::GAUGE};
    cfg.format = sp::text_format::openmetrics_2_0;
    sm::label unit_label{"__unit"};
    cfg.extra_label = unit_label("seconds");
    return prometheus_test_fixture::run_metrics_test(cfg, {},
        R"(# TYPE seastar_group_1_metric_0 gauge)" "\n"
        R"(# UNIT seastar_group_1_metric_0 seconds)" "\n"
        R"(# HELP seastar_group_1_metric_0 metric description)" "\n"
        R"(seastar_group_1_metric_0{"label-0"="label-0-0",shard="0"} 123.400000)" "\n"
        R"(# EOF)" "\n"
    );
}

SEASTAR_TEST_CASE(test_om_unit_label_name_with_unit_suffix) {
    // A name that follows the recommended <name>_<unit> convention is reported as is
    test_config cfg{data_type::COUNTER};
    cfg.format = sp::text_format::openmetrics_2_0;
    cfg.metric_name = "read_bytes";
    sm::label unit_label{"__unit"};
    cfg.extra_label = unit_label("bytes");
    return prometheus_test_fixture::run_metrics_test(cfg, {},
        R"(# TYPE seastar_group_1_read_bytes counter)" "\n"
        R"(# UNIT seastar_group_1_read_bytes bytes)" "\n"
        R"(# HELP seastar_group_1_read_bytes metric description)" "\n"
        R"(seastar_group_1_read_bytes{"label-0"="label-0-0",shard="0"} 123)" "\n"
        R"(# EOF)" "\n"
    );
}

SEASTAR_TEST_CASE(test_om_invalid_unit_label) {
    // A unit that is not made of metric name characters would corrupt the UNIT line, so it is not reported
    test_config cfg{data_type::COUNTER};
    cfg.format = sp::text_format::openmetrics_2_0;
    sm::label unit_label{"__unit"};
    cfg.extra_label = unit_label("milli seconds\n");
    return prometheus_test_fixture::run_metrics_test(cfg, {},
        R"(# TYPE seastar_group_1_metric_0 counter)" "\n"
        R"(# HELP seastar_group_1_metric_0 metric description)" "\n"
        R"(seastar_group_1_metric_0{"label-0"="label-0-0",shard="0"} 123)" "\n"
        R"(# EOF)" "\n"
    );
}

SEASTAR_TEST_CASE(test_om_unit_label_in_old_format) {
    // The 0.0.4 format ignores the __unit label like any other internal label
    test_config cfg{data_type::COUNTER};
    sm::label unit_label{"__unit"};
    cfg.extra_label = unit_label("bytes");
    return prometheus_test_fixture::run_metrics_test(cfg, {},
        R"(# HELP seastar_group_1_metric_0 metric description)" "\n"
        R"(# TYPE seastar_group_1_metric_0 counter)" "\n"
        R"(seastar_group_1_metric_0{label-0="label-0-0",shard="0"} 123)" "\n"
    );
}

SEASTAR_TEST_CASE(test_om_native_histogram) {
    // approximate_exponential_histogram<1, 1000000, 1> has schema 0 and its first bucket id is 1.
    // All of its 19 buckets are populated so they form a single span.
    // The bucket values are the per-bucket counts (not cumulative like the classic format).
    test_config cfg{data_type::HISTOGRAM};
    cfg.format = sp::text_format::openmetrics_2_0;
    return prometheus_test_fixture::run_metrics_test(cfg, {},
        R"(# TYPE seastar_group_1_metric_0 histogram)" "\n"
        R"(# HELP seastar_group_1_metric_0 metric_0)" "\n"
        R"(seastar_group_1_metric_0{"label-0"="label-0-0",shard="0"} {count:53,sum:6420722,schema:0,zero_threshold:0,zero_count:0,positive_spans:[1:19],positive_buckets:[3,3,2,3,3,2,3,3,2,3,3,2,3,2,3,3,2,3,3]})" "\n"
        R"(# EOF)" "\n"
    );
}

SEASTAR_TEST_CASE(test_om_native_histogram_aggregated) {
    test_config cfg{data_type::HISTOGRAM};
    cfg.format = sp::text_format::openmetrics_2_0;
    cfg.labels_per_metric = 0;
    cfg.aggregation_mode = aggr_mode::AGGR_SHARD_LABEL;
    return prometheus_test_fixture::run_metrics_test(cfg, {},
        R"(# TYPE seastar_group_1_metric_0 histogram)" "\n"
        R"(# HELP seastar_group_1_metric_0 metric_0)" "\n"
        R"(seastar_group_1_metric_0{} {count:53,sum:6420722,schema:0,zero_threshold:0,zero_count:0,positive_spans:[1:19],positive_buckets:[3,3,2,3,3,2,3,3,2,3,3,2,3,2,3,3,2,3,3]})" "\n"
        R"(# EOF)" "\n"
    );
}

SEASTAR_TEST_CASE(test_om_classic_histogram) {
    test_config cfg{data_type::HISTOGRAM};
    cfg.format = sp::text_format::openmetrics_2_0;
    cfg.classic_histogram = true;
    return prometheus_test_fixture::run_metrics_test(cfg, {},
        R"(# TYPE seastar_group_1_metric_0 histogram)" "\n"
        R"(# HELP seastar_group_1_metric_0 metric_0)" "\n"
        R"(seastar_group_1_metric_0{"label-0"="label-0-0",shard="0"} {count:53,sum:6420722,bucket:[2:3,4:6,8:8,16:11,32:14,64:16,128:19,256:22,512:24,1024:27,2048:30,4096:32,8192:35,16384:37,32768:40,65536:43,131072:45,262144:48,1000000:51,+Inf:53]})" "\n"
        R"(# EOF)" "\n"
    );
}

SEASTAR_TEST_CASE(test_om_summary) {
    test_config cfg{data_type::SUMMARY};
    cfg.format = sp::text_format::openmetrics_2_0;
    return prometheus_test_fixture::run_metrics_test(cfg, {},
        R"(# TYPE seastar_group_1_metric_0 summary)" "\n"
        R"(# HELP seastar_group_1_metric_0 metric_0)" "\n"
        R"(seastar_group_1_metric_0{shard="0"} {count:53,sum:6420722,quantile:[2:3,4:6,8:8,16:11,32:14,64:16,128:19,256:22,512:24,1024:27,2048:30,4096:32,8192:35,16384:37,32768:40,65536:43,131072:45,262144:48,1000000:51]})" "\n"
        R"(# EOF)" "\n"
    );
}

SEASTAR_TEST_CASE(test_om_native_histogram_with_gaps) {
    // A native histogram with empty buckets between populated ones is written as multiple spans
    // and the count of the +Inf bucket (values above the last bucket) is only reflected in count.
    co_await smp::invoke_on_all([] {
        remove_existing_metrics();
    });
    sm::metric_groups test_metrics;
    sm::histogram h;
    h.sample_count = 12;
    h.sample_sum = 100;
    // cumulative counts: buckets 0 and 1 are empty, then 2 populated, 1 empty, 3 populated, 2 above the last bucket
    h.buckets = {{0, 1.0}, {0, 2.0}, {4, 4.0}, {6, 8.0}, {6, 16.0}, {7, 32.0}, {9, 64.0}, {10, 128.0}};
    h.native_histogram = sm::native_histogram_info{.schema = 0, .min_id = 1};
    test_metrics.add_group("group-1", {
        make_histogram("metric_0", sm::description("metric_0"), {}, [h] { return h; }).aggregate({sm::shard_label})
    });

    auto actual = co_await prometheus_test_fixture::write_metrics({},
        sp::details::write_body_args{
            .filter = always_true,
            .family_filter = [](std::string_view) { return true; },
            .use_protobuf_format = false,
            .show_help = false,
            .enable_aggregation = true,
            .format = sp::text_format::openmetrics_2_0,
        });
    std::string expected =
        R"(# TYPE seastar_group_1_metric_0 histogram)" "\n"
        R"(seastar_group_1_metric_0{} {count:12,sum:100,schema:0,zero_threshold:0,zero_count:0,positive_spans:[3:2,1:3],positive_buckets:[4,2,1,2,1]})" "\n"
        R"(# EOF)" "\n";
    BOOST_REQUIRE_EQUAL(actual, expected);
}

SEASTAR_TEST_CASE(test_om_summary_without_quantiles) {
    // The quantile list is required even when it is empty, and the sum keeps all its digits
    co_await smp::invoke_on_all([] {
        remove_existing_metrics();
    });
    sm::metric_groups test_metrics;
    sm::histogram h;
    h.sample_count = 3;
    h.sample_sum = 1234567.25;
    test_metrics.add_group("group-1", {
        sm::make_summary("metric_0", sm::description("metric_0"), [h] { return h; }).aggregate({sm::shard_label})
    });

    auto actual = co_await prometheus_test_fixture::write_metrics({},
        sp::details::write_body_args{
            .filter = always_true,
            .family_filter = [](std::string_view) { return true; },
            .use_protobuf_format = false,
            .show_help = false,
            .enable_aggregation = true,
            .format = sp::text_format::openmetrics_2_0,
        });
    std::string expected =
        R"(# TYPE seastar_group_1_metric_0 summary)" "\n"
        R"(seastar_group_1_metric_0{} {count:3,sum:1234567.25,quantile:[]})" "\n"
        R"(# EOF)" "\n";
    BOOST_REQUIRE_EQUAL(actual, expected);
}

namespace {

using namespace std::chrono_literals;

const auto exemplar_t0 = std::chrono::system_clock::time_point(1520879606100ms);

// Registers one metric made by `make` and returns the output in `format`
template <typename Make>
future<std::string> write_one_metric(Make make, sp::text_format format) {
    co_await smp::invoke_on_all([] {
        remove_existing_metrics();
    });
    sm::metric_groups test_metrics;
    test_metrics.add_group("group-1", {make()});
    co_return co_await prometheus_test_fixture::write_metrics({},
        sp::details::write_body_args{
            .filter = always_true,
            .family_filter = [](std::string_view) { return true; },
            .use_protobuf_format = false,
            .show_help = false,
            .enable_aggregation = false,
            .format = format,
        });
}

// Given out of time order, as the exemplars of different shards are after aggregation
std::vector<sm::exemplar> out_of_order_exemplars() {
    return {
        {{{"trace_id", "b"}}, 12.5, exemplar_t0 + 1s},
        {{{"trace_id", "a"}, {"span_id", "s\"1"}}, 9, exemplar_t0},
    };
}

sm::histogram summary_with_exemplars() {
    sm::histogram h;
    h.sample_count = 10;
    h.sample_sum = 20;
    h.buckets = {{3, 0.5}, {9, 0.99}};
    h.exemplars = out_of_order_exemplars();
    return h;
}

}

SEASTAR_TEST_CASE(test_om_histogram_exemplars) {
    // Exemplars follow the composite value in timestamp order, with escaped label values
    auto actual = co_await write_one_metric([] {
        return sm::make_histogram("metric_0", sm::description("metric_0"), [] {
            sm::histogram h;
            h.sample_count = 2;
            h.sample_sum = 3;
            h.buckets = {{1, 1.0}, {2, 2.0}};
            h.exemplars = out_of_order_exemplars();
            return h;
        });
    }, sp::text_format::openmetrics_2_0);
    std::string expected =
        R"(# TYPE seastar_group_1_metric_0 histogram)" "\n"
        R"(seastar_group_1_metric_0{shard="0"} {count:2,sum:3,bucket:[1:1,2:2,+Inf:2]})"
        R"( # {span_id="s\"1",trace_id="a"} 9 1520879606.100)"
        R"( # {trace_id="b"} 12.5 1520879607.100)" "\n"
        R"(# EOF)" "\n";
    BOOST_REQUIRE_EQUAL(actual, expected);
}

SEASTAR_TEST_CASE(test_om_exemplars_same_timestamp) {
    // Exemplars with the same millisecond timestamp are written in value order, as Prometheus orders them
    auto actual = co_await write_one_metric([] {
        return sm::make_histogram("metric_0", sm::description("metric_0"), [] {
            sm::histogram h;
            h.sample_count = 2;
            h.sample_sum = 3;
            h.buckets = {{1, 1.0}, {2, 2.0}};
            h.exemplars = {
                {{{"trace_id", "a"}}, 2, exemplar_t0},
                {{{"trace_id", "b"}}, 1, exemplar_t0 + 100us},
            };
            return h;
        });
    }, sp::text_format::openmetrics_2_0);
    std::string expected =
        R"(# TYPE seastar_group_1_metric_0 histogram)" "\n"
        R"(seastar_group_1_metric_0{shard="0"} {count:2,sum:3,bucket:[1:1,2:2,+Inf:2]})"
        R"( # {trace_id="b"} 1 1520879606.100)"
        R"( # {trace_id="a"} 2 1520879606.100)" "\n"
        R"(# EOF)" "\n";
    BOOST_REQUIRE_EQUAL(actual, expected);
}

SEASTAR_TEST_CASE(test_om_exemplar_with_invalid_label_name) {
    // Exemplar label names that are not valid label names are quoted and escaped
    auto actual = co_await write_one_metric([] {
        return sm::make_histogram("metric_0", sm::description("metric_0"), [] {
            sm::histogram h;
            h.sample_count = 2;
            h.sample_sum = 3;
            h.buckets = {{1, 1.0}, {2, 2.0}};
            h.exemplars = {
                {{{"trace id", "a"}}, 1, exemplar_t0},
                {{{"trace_id\"", "b"}}, 1, exemplar_t0},
                {{{"1trace_id", "c"}}, 1, exemplar_t0},
                {{{"trace_id", "d"}}, 2, exemplar_t0 + 1s},
            };
            return h;
        });
    }, sp::text_format::openmetrics_2_0);
    std::string expected =
        R"(# TYPE seastar_group_1_metric_0 histogram)" "\n"
        R"(seastar_group_1_metric_0{shard="0"} {count:2,sum:3,bucket:[1:1,2:2,+Inf:2]})"
        R"( # {"trace id"="a"} 1 1520879606.100 # {"trace_id\""="b"} 1 1520879606.100)"
        R"( # {"1trace_id"="c"} 1 1520879606.100 # {trace_id="d"} 2 1520879607.100)" "\n"
        R"(# EOF)" "\n";
    BOOST_REQUIRE_EQUAL(actual, expected);
}

SEASTAR_TEST_CASE(test_om_summary_exemplars_not_written) {
    // OpenMetrics does not define exemplars for summaries, Prometheus drops them
    auto actual = co_await write_one_metric([] {
        return sm::make_summary("metric_0", sm::description("metric_0"), [] { return summary_with_exemplars(); });
    }, sp::text_format::openmetrics_2_0);
    std::string expected =
        R"(# TYPE seastar_group_1_metric_0 summary)" "\n"
        R"(seastar_group_1_metric_0{shard="0"} {count:10,sum:20,quantile:[0.5:3,0.99:9]})" "\n"
        R"(# EOF)" "\n";
    BOOST_REQUIRE_EQUAL(actual, expected);
}

SEASTAR_TEST_CASE(test_om_histogram_exemplars_aggregated_over_shards) {
    // A per node histogram, the sum of the shard histograms, has the exemplars of all the shards.
    // Shards with a higher id have older exemplars, so they are written first.
    static thread_local std::unique_ptr<sm::metric_groups> shard_metrics;
    co_await smp::invoke_on_all([] {
        remove_existing_metrics();
        shard_metrics = std::make_unique<sm::metric_groups>();
        shard_metrics->add_group("group-1", {
            sm::make_histogram("metric_0", sm::description("metric_0"), [] {
                sm::histogram h;
                h.sample_count = 1;
                h.sample_sum = 1;
                h.buckets = {{1, 1.0}};
                h.exemplars = {{{{"trace_id", fmt::format("shard-{}", this_shard_id())}}, double(this_shard_id()),
                        exemplar_t0 - std::chrono::seconds(this_shard_id())}};
                return h;
            }).aggregate({sm::shard_label})
        });
    });

    auto actual = co_await prometheus_test_fixture::write_metrics({},
        sp::details::write_body_args{
            .filter = always_true,
            .family_filter = [](std::string_view) { return true; },
            .use_protobuf_format = false,
            .show_help = false,
            .enable_aggregation = true,
            .format = sp::text_format::openmetrics_2_0,
        });
    co_await smp::invoke_on_all([] {
        shard_metrics.reset();
    });

    auto shards = this_smp_shard_count();
    std::string expected = fmt::format("# TYPE seastar_group_1_metric_0 histogram\n"
            "seastar_group_1_metric_0{{}} {{count:{0},sum:{0},bucket:[1:{0},+Inf:{0}]}}", shards);
    for (auto shard = shards; shard-- > 0;) {
        expected += fmt::format(R"( # {{trace_id="shard-{0}"}} {0} {1:.3f})", shard,
                std::chrono::duration<double>((exemplar_t0 - std::chrono::seconds(shard)).time_since_epoch()).count());
    }
    expected += "\n# EOF\n";
    BOOST_REQUIRE_EQUAL(actual, expected);
}

SEASTAR_TEST_CASE(test_exemplars_ignored_in_old_format) {
    auto actual = co_await write_one_metric([] {
        return sm::make_summary("metric_0", sm::description("metric_0"), [] { return summary_with_exemplars(); });
    }, sp::text_format::prometheus_0_0_4);
    std::string expected =
        R"(# TYPE seastar_group_1_metric_0 summary)" "\n"
        R"(seastar_group_1_metric_0_sum{shard="0"} 20)" "\n"
        R"(seastar_group_1_metric_0_count{shard="0"} 10)" "\n"
        R"(seastar_group_1_metric_0{quantile="0.500000",shard="0"} 3)" "\n"
        R"(seastar_group_1_metric_0{quantile="0.990000",shard="0"} 9)" "\n";
    BOOST_REQUIRE_EQUAL(actual, expected);
}

SEASTAR_TEST_CASE(test_om_histogram_bounds) {
    // Bounds keep their precision, a -Inf bucket is written as -Inf, and a +Inf bucket is only written once
    co_await smp::invoke_on_all([] {
        remove_existing_metrics();
    });
    sm::metric_groups test_metrics;
    sm::histogram h;
    h.sample_count = 3;
    h.sample_sum = 1;
    h.buckets = {{0, -std::numeric_limits<double>::infinity()}, {1, 1e-7}, {2, 5e-7}, {3, std::numeric_limits<double>::infinity()}};
    test_metrics.add_group("group-1", {
        make_histogram("metric_0", sm::description("metric_0"), {}, [h] { return h; }).aggregate({sm::shard_label})
    });

    auto actual = co_await prometheus_test_fixture::write_metrics({},
        sp::details::write_body_args{
            .filter = always_true,
            .family_filter = [](std::string_view) { return true; },
            .use_protobuf_format = false,
            .show_help = false,
            .enable_aggregation = true,
            .format = sp::text_format::openmetrics_2_0,
        });
    std::string expected =
        R"(# TYPE seastar_group_1_metric_0 histogram)" "\n"
        R"(seastar_group_1_metric_0{} {count:3,sum:1,bucket:[-Inf:0,1e-07:1,5e-07:2,+Inf:3]})" "\n"
        R"(# EOF)" "\n";
    BOOST_REQUIRE_EQUAL(actual, expected);
}

SEASTAR_TEST_CASE(test_openmetrics_2_0_accept_quality) {
    auto quality = sp::details::openmetrics_2_0_accept_quality;
    BOOST_REQUIRE_EQUAL(quality("application/openmetrics-text; version=2.0.0"), 1);
    BOOST_REQUIRE_EQUAL(quality("application/openmetrics-text;version=2.0.0;charset=utf-8"), 1);
    BOOST_REQUIRE_EQUAL(quality("application/openmetrics-text; version=2.0.0; q=0.9, text/plain; version=0.0.4; q=0.5"), 0.9);
    BOOST_REQUIRE_EQUAL(quality("text/plain;version=0.0.4;q=0.5, application/openmetrics-text; version=2.0.0; q=0.9"), 0.9);
    // media types and parameters are case-insensitive
    BOOST_REQUIRE_EQUAL(quality("Application/OpenMetrics-Text; Version=2.0.0; Q=0.8"), 0.8);
    BOOST_REQUIRE_EQUAL(quality(""), 0);
    BOOST_REQUIRE_EQUAL(quality("*/*"), 0);
    BOOST_REQUIRE_EQUAL(quality("text/plain; version=0.0.4"), 0);
    // older OpenMetrics versions are not supported
    BOOST_REQUIRE_EQUAL(quality("application/openmetrics-text; version=1.0.0"), 0);
    BOOST_REQUIRE_EQUAL(quality("application/openmetrics-text"), 0);
    // the version must belong to the openmetrics media type
    BOOST_REQUIRE_EQUAL(quality("text/plain; version=2.0.0"), 0);
    BOOST_REQUIRE_EQUAL(quality("application/vnd.google.protobuf; proto=io.prometheus.client.MetricFamily; encoding=delimited"), 0);
    // q=0 means the client refuses the media type
    BOOST_REQUIRE_EQUAL(quality("application/openmetrics-text;version=2.0.0;q=0, text/plain;version=0.0.4;q=1"), 0);
    BOOST_REQUIRE_EQUAL(quality("application/openmetrics-text; version=2.0.0; q=0.000"), 0);
    BOOST_REQUIRE_EQUAL(quality("application/openmetrics-text; version=2.0.0; q=0.001"), 0.001);
    // a parameter value may be quoted
    BOOST_REQUIRE_EQUAL(quality(R"(application/openmetrics-text; version="2.0.0"; q="0.5")"), 0.5);
    // a quality value is between 0 and 1
    BOOST_REQUIRE_EQUAL(quality("application/openmetrics-text; version=2.0.0; q=2"), 1);
    BOOST_REQUIRE_EQUAL(quality("application/openmetrics-text; version=2.0.0; q=-1"), 0);
    return make_ready_future<>();
}

SEASTAR_TEST_CASE(test_text_0_0_4_accept_quality) {
    auto quality = sp::details::text_0_0_4_accept_quality;
    BOOST_REQUIRE_EQUAL(quality("text/plain"), 1);
    BOOST_REQUIRE_EQUAL(quality("text/plain; version=0.0.4; charset=utf-8"), 1);
    BOOST_REQUIRE_EQUAL(quality("application/openmetrics-text;version=2.0.0;q=0.5,text/plain;version=0.0.4;q=0.3"), 0.3);
    BOOST_REQUIRE_EQUAL(quality("Text/Plain; Version=\"0.0.4\"; Q=0.6"), 0.6);
    BOOST_REQUIRE_EQUAL(quality(""), 0);
    BOOST_REQUIRE_EQUAL(quality("*/*"), 0);
    BOOST_REQUIRE_EQUAL(quality("text/plain; version=1.0.0"), 0);
    BOOST_REQUIRE_EQUAL(quality("application/openmetrics-text; version=2.0.0"), 0);
    BOOST_REQUIRE_EQUAL(quality("text/plain;version=0.0.4;q=0"), 0);
    return make_ready_future<>();
}

SEASTAR_TEST_CASE(test_protobuf_accept_quality) {
    auto quality = sp::details::protobuf_accept_quality;
    BOOST_REQUIRE_EQUAL(quality("application/vnd.google.protobuf; proto=io.prometheus.client.MetricFamily; encoding=delimited"), 1);
    BOOST_REQUIRE_EQUAL(quality("application/vnd.google.protobuf;proto=io.prometheus.client.MetricFamily;encoding=delimited;q=0.7,text/plain;version=0.0.4;q=0.3"), 0.7);
    BOOST_REQUIRE_EQUAL(quality("text/plain;version=0.0.4;q=0.3, application/vnd.google.protobuf;proto=io.prometheus.client.MetricFamily;encoding=delimited;q=0.7"), 0.7);
    BOOST_REQUIRE_EQUAL(quality(""), 0);
    BOOST_REQUIRE_EQUAL(quality("text/plain; version=0.0.4"), 0);
    BOOST_REQUIRE_EQUAL(quality("application/openmetrics-text; version=2.0.0"), 0);
    // the protobuf media type is only meaningful with its parameters
    BOOST_REQUIRE_EQUAL(quality("application/vnd.google.protobuf"), 0);
    BOOST_REQUIRE_EQUAL(quality("application/vnd.google.protobuf;proto=io.prometheus.client.MetricFamily;encoding=delimited;q=0,text/plain;version=0.0.4"), 0);
    return make_ready_future<>();
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

