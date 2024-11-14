#include <seastar/core/metrics.hh>
#include <seastar/core/metrics_api.hh>
#include <seastar/core/metrics_registration.hh>
#include <seastar/testing/test_case.hh>
#include <seastar/testing/test_runner.hh>
#include <seastar/testing/thread_test_case.hh>

namespace sm = seastar::metrics;
namespace smi = seastar::metrics::impl;

bool metric_family_exists(int handle, const seastar::sstring& name) {
    return smi::get_value_map(handle).contains(name);
}

void assert_metric_families_equivalent(int source, int destination,
                                       const seastar::sstring& name) {
    const auto& source_value_map = smi::get_value_map(source);
    const auto& destination_value_map = smi::get_value_map(destination);

    BOOST_REQUIRE(source_value_map.contains(name));
    BOOST_REQUIRE(destination_value_map.contains(name));

    const auto& source_family = source_value_map.at(name);
    const auto& destination_family = destination_value_map.at(name);
    for (const auto& [labels, source_metric]: source_family) {
        auto replica_iter = destination_family.find(labels.labels());
        BOOST_REQUIRE(replica_iter != destination_family.end());
        
        const auto& replica_metric = replica_iter->second;
        BOOST_REQUIRE(source_metric->get_id() == replica_metric->get_id());

        auto source_current_value = source_metric->get_function()().i();
        auto replica_current_value = replica_metric->get_function()().i();
        BOOST_REQUIRE(source_current_value == replica_current_value);
    }
}

SEASTAR_THREAD_TEST_CASE(replicate_metrics_test) {
    int foo_handle = sm::default_handle();
    sm::metric_groups foo(foo_handle);
    foo.add_group("a", {
        sm::make_gauge(
            "gauge",
            [] { return 0; })});

    int bar_handle = sm::default_handle() + 1;
    int baz_handle = sm::default_handle() + 2;
    sm::replicate_metric_families(foo_handle, {
            {"a_gauge", bar_handle},
            {"a_gauge", baz_handle}
    }).get();

    assert_metric_families_equivalent(foo_handle, bar_handle, "a_gauge");
    assert_metric_families_equivalent(foo_handle, baz_handle, "a_gauge");
}

SEASTAR_THREAD_TEST_CASE(replicate_same_metric_test) {
    int foo_handle = sm::default_handle();

    sm::metric_groups foo(foo_handle);
    foo.add_group("a", {
        sm::make_gauge(
            "x",
            [] { return 0; },
            sm::description("a_x_description"),
            {sm::label("id")("1")}),
        sm::make_gauge(
            "x",
            [] { return 0; },
            sm::description("a_x_description"),
            {sm::label("id")("2")}),
        sm::make_gauge(
            "y",
            [] { return 0; },
            sm::description("a_y_description"),
            {sm::label("id")("1")}),
    });

    int bar_handle = sm::default_handle() + 1;
    sm::metric_groups bar(bar_handle);

    // Test that subsequent attempts to replicate the same metric
    // family are ignored.
    sm::replicate_metric_families(foo_handle, {{"a_x", bar_handle}}).get();
    assert_metric_families_equivalent(foo_handle, bar_handle, "a_x");
    sm::replicate_metric_families(foo_handle, {{"a_x", bar_handle}}).get();
    assert_metric_families_equivalent(foo_handle, bar_handle, "a_x");


    // Ensure that when the set of replicated metric families is changed
    // the replicas that are not in the new set are removed.
    sm::replicate_metric_families(foo_handle, {{"a_y", bar_handle}}).get();
    assert_metric_families_equivalent(foo_handle, bar_handle, "a_y");

    BOOST_REQUIRE(metric_family_exists(bar_handle, "a_y"));
}
