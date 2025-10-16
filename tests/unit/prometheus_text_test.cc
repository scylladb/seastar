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

#include "memory_data_sink.hh"

#include <sstream>
#include <string_view>


using namespace seastar;
using namespace httpd;
using namespace std::literals;

namespace sm = seastar::metrics;
namespace sp = seastar::prometheus;

thread_local auto impl_ = sm::impl::get_local_impl();

namespace {
[[maybe_unused]] void remove_existing_metrics() {
    const auto& map = seastar::metrics::impl::get_value_map();

    for (auto& family : map) {
        auto name = family.first;
        for (auto& series: family.second) {
            seastar::metrics::impl::unregister_metric(series.second->get_id());
        }
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

        using access = prometheus::details::test_access;

        std::stringstream ss;
        output_stream<char> out{data_sink{std::make_unique<memory_data_sink_impl>(ss, 10)}};
        auto filter = test_conf.filter.value_or(always_true);
        co_await access{}.write_body(config,
            false,
            "",    // metric family name
            false, // use protobuf
            test_conf.show_help,
            test_conf.aggregation_mode != aggr_mode::NO_AGGR,
            filter,
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

