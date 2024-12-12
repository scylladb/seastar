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
 * Copyright (C) 2019 ScyllaDB.
 */

#include <seastar/core/metrics_registration.hh>
#include <seastar/core/metrics.hh>
#include <seastar/core/metrics_api.hh>
#include <seastar/core/relabel_config.hh>
#include <seastar/core/scheduling.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/sharded.hh>
#include <seastar/core/do_with.hh>
#include <seastar/core/io_queue.hh>
#include <seastar/core/loop.hh>
#include <seastar/core/internal/estimated_histogram.hh>
#include <seastar/testing/random.hh>
#include <seastar/testing/test_case.hh>
#include <seastar/testing/thread_test_case.hh>
#include <seastar/testing/test_runner.hh>
#include <boost/range/irange.hpp>
#include <ranges>

SEASTAR_TEST_CASE(test_add_group) {
    using namespace seastar::metrics;
    // Just has to compile:
    metric_groups()
        .add_group("g1", {})
        .add_group("g2", std::vector<metric_definition>());
    return seastar::make_ready_future();
}

/**
 * This function return the different name label values
 *  for the named metric.
 *
 *  @note: If the statistic or label doesn't exist, the test
 *  that calls this function will fail.
 *
 * @param metric_name - the metric name
 * @param label_name - the label name
 * @return a set containing all the different values
 *         of the label.
 */
static std::set<seastar::sstring> get_label_values(seastar::sstring metric_name, seastar::sstring label_name) {
    namespace smi = seastar::metrics::impl;
    auto all_metrics = smi::get_values();
    const auto& all_metadata = *all_metrics->metadata;
    const auto qp_group = find_if(cbegin(all_metadata), cend(all_metadata),
        [&metric_name] (const auto& x) { return x.mf.name == metric_name; });
    BOOST_REQUIRE(qp_group != cend(all_metadata));
    std::set<seastar::sstring> labels;
    for (const auto& metric : qp_group->metrics) {
        const auto found = metric.labels().find(label_name);
        BOOST_REQUIRE(found != metric.labels().cend());
        labels.insert(found->second);
    }
    return labels;
}

SEASTAR_THREAD_TEST_CASE(test_renaming_scheuling_groups) {
    // this seams a little bit out of place but the
    // renaming functionality is primarily for statistics
    // otherwise those classes could have just been reused
    // without renaming them.
    using namespace seastar;

    static const char* name1 = "A";
    static const char* name2 = "B";
    scheduling_group sg =  create_scheduling_group("hello", 111).get();
    auto rng = std::views::iota(0, 1000);
    // repeatedly change the group name back and forth in
    // decresing time intervals to see if it generate double
    //registration statistics errors.
    for (auto&& i : rng) {
        const char* name = i%2 ? name1 : name2;
        const char* prev_name = i%2 ? name2 : name1;
        sleep(std::chrono::microseconds(100000/(i+1))).get();
        rename_scheduling_group(sg, name).get();
        std::set<sstring> label_vals = get_label_values(sstring("scheduler_shares"), sstring("group"));
        // validate that the name that we *renamed to* is in the stats
        BOOST_REQUIRE(label_vals.find(sstring(name)) != label_vals.end());
        // validate that the name that we *renamed from* is *not* in the stats
        BOOST_REQUIRE(label_vals.find(sstring(prev_name)) == label_vals.end());
    }

    smp::invoke_on_all([sg] () {
        return do_with(std::uniform_int_distribution<int>(), boost::irange<int>(0, 1000),
                [sg] (std::uniform_int_distribution<int>& dist, boost::integer_range<int>& rng) {
            // flip a fair coin and rename to one of two options and rename to that
            // scheduling group name, do it 1000 in parallel on all shards so there
            // is a chance of collision.
            return do_for_each(rng, [sg, &dist] (auto i) {
                bool odd = dist(seastar::testing::local_random_engine)%2;
                return rename_scheduling_group(sg, odd ? name1 : name2);
            });
        });
    }).get();

    std::set<sstring> label_vals = get_label_values(sstring("scheduler_shares"), sstring("group"));
    // validate that only one of the names is eventually in the metrics
    bool name1_found = label_vals.find(sstring(name1)) != label_vals.end();
    bool name2_found = label_vals.find(sstring(name2)) != label_vals.end();
    BOOST_REQUIRE((name1_found && !name2_found) || (name2_found && !name1_found));
}

int count_by_label(const std::string& label) {
    seastar::foreign_ptr<seastar::metrics::impl::values_reference> values = seastar::metrics::impl::get_values();
    int count = 0;
    for (auto&& md : (*values->metadata)) {
        for (auto&& mi : md.metrics) {
            if (label == "" || mi.labels().find(label) != mi.labels().end()) {
                count++;
            }
        }
    }
    return count;
}

int count_by_fun(std::function<bool(const seastar::metrics::impl::metric_series_metadata&)> f) {
    seastar::foreign_ptr<seastar::metrics::impl::values_reference> values = seastar::metrics::impl::get_values();
    int count = 0;
    for (auto&& md : (*values->metadata)) {
        for (auto&& mi : md.metrics) {
            if (f(mi)) {
                count++;
            }
        }
    }
    return count;
}

SEASTAR_THREAD_TEST_CASE(test_relabel_add_labels) {
    using namespace seastar::metrics;
    namespace sm = seastar::metrics;
    sm::metric_groups app_metrics;
    app_metrics.add_group("test", {
        sm::make_gauge("gauge_1", sm::description("gague 1"), [] { return 0; }),
        sm::make_counter("counter_1", sm::description("counter 1"), [] { return 1; })
    });

    std::vector<sm::relabel_config> rl(1);
    rl[0].source_labels = {"__name__"};
    rl[0].target_label = "level";
    rl[0].replacement = "1";
    rl[0].expr = "test_counter_.*";

    sm::metric_relabeling_result success = sm::set_relabel_configs(rl).get();
    BOOST_CHECK_EQUAL(success.metrics_relabeled_due_to_collision, 0);
    BOOST_CHECK_EQUAL(count_by_label("level"), 1);
    app_metrics.add_group("test", {
        sm::make_counter("counter_2", sm::description("counter 2"), [] { return 2; })
    });
    BOOST_CHECK_EQUAL(count_by_label("level"), 2);
    sm::set_relabel_configs({}).get();
}

SEASTAR_THREAD_TEST_CASE(test_metrics_family_aggregate) {
    using namespace seastar::metrics;
    namespace sm = seastar::metrics;
    sm::metric_groups app_metrics;
    sm::label lb("lb");
    app_metrics.add_group("test", {
        sm::make_gauge("gauge_1", sm::description("gague 1"), [] { return 1; })(lb("1")),
        sm::make_gauge("gauge_1", sm::description("gague 1"), [] { return 2; })(lb("2")),
        sm::make_counter("counter_1", sm::description("counter 1"), [] { return 3; })(lb("1")),
        sm::make_counter("counter_1", sm::description("counter 1"), [] { return 4; })(lb("2"))
    });
    std::vector<sm::relabel_config> rl(2);
    rl[0].source_labels = {"__name__"};
    rl[0].action = sm::relabel_config::relabel_action::drop;

    rl[1].source_labels = {"lb"};
    rl[1].action = sm::relabel_config::relabel_action::keep;
    // Dropping the lev label would cause a conflict, but not crash the system
    sm::set_relabel_configs(rl).get();

    std::vector<sm::metric_family_config> fc(2);
    fc[0].name = "test_gauge_1";
    fc[0].aggregate_labels = { "lb" };
    fc[1].regex_name = "test_gauge1.*";
    fc[1].aggregate_labels = { "ll", "aa" };
    sm::set_metric_family_configs(fc);
    seastar::foreign_ptr<seastar::metrics::impl::values_reference> values = seastar::metrics::impl::get_values();
    int count = 0;
    for (auto&& md : (*values->metadata)) {
        if (md.mf.name == "test_gauge_1") {
            BOOST_CHECK_EQUAL(md.mf.aggregate_labels.size(), 1);
            BOOST_CHECK_EQUAL(md.mf.aggregate_labels[0], "lb");
        } else {
            BOOST_CHECK_EQUAL(md.mf.aggregate_labels.size(), 0);
        }
        count++;
    }
    BOOST_CHECK_EQUAL(count, 2);
    app_metrics.add_group("test", {
        sm::make_gauge("gauge1_1", sm::description("gague 1"), [] { return 1; })(lb("1")),
        sm::make_gauge("gauge1_1", sm::description("gague 1"), [] { return 2; })(lb("2")),
        sm::make_counter("counter1_1", sm::description("counter 1"), [] { return 3; })(lb("1")),
        sm::make_counter("counter1_1", sm::description("counter 1"), [] { return 4; })(lb("2"))
    });
    values = seastar::metrics::impl::get_values();
    count = 0;
    for (auto&& md : (*values->metadata)) {
        if (md.mf.name == "test_gauge_1") {
            BOOST_CHECK_EQUAL(md.mf.aggregate_labels.size(), 1);
            BOOST_CHECK_EQUAL(md.mf.aggregate_labels[0], "lb");
        } else if (md.mf.name == "test_gauge1_1") {
            BOOST_CHECK_EQUAL(md.mf.aggregate_labels.size(), 2);
            BOOST_CHECK_EQUAL(md.mf.aggregate_labels[0], "ll");
        } else {
            BOOST_CHECK_EQUAL(md.mf.aggregate_labels.size(), 0);
        }
        count++;
    }
    BOOST_CHECK_EQUAL(count, 4);
    std::vector<sm::relabel_config> rl1;
    sm::set_relabel_configs(rl1).get();
}

SEASTAR_THREAD_TEST_CASE(test_relabel_drop_label_prevent_runtime_conflicts) {
    using namespace seastar::metrics;
    namespace sm = seastar::metrics;
    sm::metric_groups app_metrics;
    app_metrics.add_group("test2", {
        sm::make_gauge("gauge_1", sm::description("gague 1"), { sm::label_instance("g", "1")}, [] { return 0; }),
        sm::make_counter("counter_1", sm::description("counter 1"), [] { return 0; }),
        sm::make_counter("counter_1", sm::description("counter 1"), { sm::label_instance("lev", "2")}, [] { return 0; })
    });
    BOOST_CHECK_EQUAL(count_by_label("lev"), 1);

    std::vector<sm::relabel_config> rl(1);
    rl[0].source_labels = {"lev"};
    rl[0].expr = "2";
    rl[0].target_label = "lev";
    rl[0].action = sm::relabel_config::relabel_action::drop_label;
    // Dropping the lev label would cause a conflict, but not crash the system
    sm::metric_relabeling_result success = sm::set_relabel_configs(rl).get();
    BOOST_CHECK_EQUAL(success.metrics_relabeled_due_to_collision, 1);
    BOOST_CHECK_EQUAL(count_by_label("lev"), 0);
    BOOST_CHECK_EQUAL(count_by_label("err"), 1);

    //reseting all the labels to their original state
    success = sm::set_relabel_configs({}).get();
    BOOST_CHECK_EQUAL(success.metrics_relabeled_due_to_collision, 0);
    BOOST_CHECK_EQUAL(count_by_label("lev"), 1);
    BOOST_CHECK_EQUAL(count_by_label("err"), 0);
    sm::set_relabel_configs({}).get();
}

SEASTAR_THREAD_TEST_CASE(test_relabel_enable_disable_skip_when_empty) {
    using namespace seastar::metrics;
    namespace sm = seastar::metrics;
    sm::metric_groups app_metrics;
    app_metrics.add_group("test3", {
        sm::make_gauge("gauge_1", sm::description("gague 1"), { sm::label_instance("lev3", "3")}, [] { return 0; }),
        sm::make_counter("counter_1", sm::description("counter 1"), { sm::label_instance("lev3", "3")}, [] { return 0; }),
        sm::make_counter("counter_2", sm::description("counter 2"), { sm::label_instance("lev3", "3")}, [] { return 0; })
    });
    std::vector<sm::relabel_config> rl(2);
    rl[0].source_labels = {"__name__"};
    rl[0].action = sm::relabel_config::relabel_action::drop;

    rl[1].source_labels = {"lev3"};
    rl[1].expr = "3";
    rl[1].action = sm::relabel_config::relabel_action::keep;
    // We just disable all metrics besides those mark as lev3
    sm::metric_relabeling_result success = sm::set_relabel_configs(rl).get();
    BOOST_CHECK_EQUAL(success.metrics_relabeled_due_to_collision, 0);
    BOOST_CHECK_EQUAL(count_by_label(""), 3);
    BOOST_CHECK_EQUAL(count_by_fun([](const seastar::metrics::impl::metric_series_metadata& mi) {
        return mi.should_skip_when_empty() == sm::skip_when_empty::yes;
    }), 0);

    std::vector<sm::relabel_config> rl2(3);
    rl2[0].source_labels = {"__name__"};
    rl2[0].action = sm::relabel_config::relabel_action::drop;

    rl2[1].source_labels = {"lev3"};
    rl2[1].expr = "3";
    rl2[1].action = sm::relabel_config::relabel_action::keep;

    rl2[2].source_labels = {"__name__"};
    rl2[2].expr = "test3.*";
    rl2[2].action = sm::relabel_config::relabel_action::skip_when_empty;

    success = sm::set_relabel_configs(rl2).get();
    BOOST_CHECK_EQUAL(success.metrics_relabeled_due_to_collision, 0);
    BOOST_CHECK_EQUAL(count_by_label(""), 3);
    BOOST_CHECK_EQUAL(count_by_fun([](const seastar::metrics::impl::metric_series_metadata& mi) {
        return mi.should_skip_when_empty() == sm::skip_when_empty::yes;
    }), 3);
    // clear the configuration
    success = sm::set_relabel_configs({}).get();
    app_metrics.add_group("test3", {
        sm::make_counter("counter_3", sm::description("counter 2"), { sm::label_instance("lev3", "3")}, [] { return 0; })(sm::skip_when_empty::yes)
    });
    std::vector<sm::relabel_config> rl3(3);
    rl3[0].source_labels = {"__name__"};
    rl3[0].action = sm::relabel_config::relabel_action::drop;

    rl3[1].source_labels = {"lev3"};
    rl3[1].expr = "3";
    rl3[1].action = sm::relabel_config::relabel_action::keep;

    rl3[2].source_labels = {"__name__"};
    rl3[2].expr = "test3.*";
    rl3[2].action = sm::relabel_config::relabel_action::report_when_empty;

    success = sm::set_relabel_configs(rl3).get();
    BOOST_CHECK_EQUAL(success.metrics_relabeled_due_to_collision, 0);
    BOOST_CHECK_EQUAL(count_by_fun([](const seastar::metrics::impl::metric_series_metadata& mi) {
        return mi.should_skip_when_empty() == sm::skip_when_empty::yes;
    }), 0);
    sm::set_relabel_configs({}).get();
}

SEASTAR_THREAD_TEST_CASE(test_estimated_histogram) {
    using namespace seastar::metrics;
    using namespace std::chrono_literals;
    internal::time_estimated_histogram histogram1;
    internal::time_estimated_histogram histogram2;
    // The number of linearly-spaced buckets between consecutive powers of 2 in time_estimated_histogram.
    constexpr int PRECISION = 4;

    // The lower bound of time_estimated_histogram is 512 us.
    std::chrono::steady_clock::duration min = std::chrono::microseconds(512);
    std::chrono::steady_clock::duration next = min*2;

    for (size_t i = 0; i < 16; i++) {
        auto delta = (next - min)/PRECISION;
        for (size_t j = 0; j< PRECISION; j++) {
            histogram1.add(min + delta*j);
        }
        min = next;
        next *= 2;
    }
    BOOST_CHECK_EQUAL(histogram1.count(), 64);
    for (size_t i = 0; i < 64; i++) {
        BOOST_CHECK_EQUAL(histogram1.get(i), 1);
    }
    min = std::chrono::microseconds(512);
    next = min*2;
    for (size_t i = 0; i < 8; i++) {
        auto delta = (next - min)/PRECISION;
        for (size_t j = 0; j< PRECISION; j++) {
            histogram2.add(min + delta*j);
        }
        min = next;
        next *= 2;
    }
    BOOST_CHECK_EQUAL(histogram2.count(), 32);
    for (size_t i = 0; i < 32; i++) {
        BOOST_CHECK_EQUAL(histogram2.get(i), 1);
    }
    for (size_t i = 33; i < 64; i++) {
        BOOST_CHECK_EQUAL(histogram2.get(i), 0);
    }
    histogram1.merge(histogram2);
    BOOST_CHECK_EQUAL(histogram1.count(), 96);
    for (size_t i = 0; i < 32; i++) {
        BOOST_CHECK_EQUAL(histogram1.get(i), 2);
    }
    for (size_t i = 33; i < 64; i++) {
        BOOST_CHECK_EQUAL(histogram1.get(i), 1);
    }
    auto mh = histogram1.to_metrics_histogram();
    for (size_t i = 0; i < 32; i++) {
        BOOST_CHECK_EQUAL(mh.buckets[i].count, 2 + i*2);
    }
    for (size_t i = 33; i < 64; i++) {
        BOOST_CHECK_EQUAL(mh.buckets[i].count, 33 + i);
    }
}
