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

#include "core/do_with.hh"
#include "test-utils.hh"
#include "core/sstring.hh"
#include "core/reactor.hh"
#include "core/do_with.hh"
#include "core/future-util.hh"
#include "core/sharded.hh"
#include "core/gate.hh"
#include "core/scollectd.hh"
#include "core/scollectd_api.hh"

using namespace seastar;
using namespace scollectd;

static const plugin_id  plugin = "the_piglet";
static const plugin_instance_id my_id = "wulf";

template<typename... _Args>
static void test_bind_counters(_Args&& ... args) {
    std::vector<type_instance_id> bound;

    {
        plugin_instance_metrics pm(plugin, my_id, std::forward<_Args>(args)...);

        bound = pm.bound_ids();
        BOOST_CHECK_EQUAL(bound.size(), sizeof...(args));

        for (auto& id : bound) {
            auto vals = get_collectd_value(id);
            BOOST_CHECK_GE(vals.size(), 1);
        }
    }

    // check unregistration
    for (auto& id : bound) {
        auto vals = get_collectd_value(id);
        BOOST_CHECK_EQUAL(vals.size(), 0);
    }
}

SEASTAR_TEST_CASE(test_simple_plugin_instance_metrics) {
    uint64_t counter = 12;
    test_bind_counters(total_bytes("test_bytes", counter));

    double cow;
    test_bind_counters(total_bytes("test_bytes", counter), typed_value_impl<known_type::uptime>("sleeping", cow));

    int64_t rx, tx;
    test_bind_counters(typed_value_impl<known_type::io_packets>("christmas_presents", rx, tx));

    // comment/description
    int64_t woff;
    test_bind_counters(typed_value_impl<known_type::connections>("woffs", description("this is how many woffs where barked"), woff));

    // typed
    test_bind_counters(typed_value_impl<known_type::connections>("waffs", make_typed(data_type::ABSOLUTE, woff)));

    return make_ready_future();
}

SEASTAR_TEST_CASE(test_simple_description) {
    uint64_t counter = 12;
    sstring desc = "I am hungry like the wolf";

    plugin_instance_metrics pm(plugin, my_id, total_bytes("test_bytes", description(desc), counter));

    auto s = get_collectd_description_str(pm.bound_ids().front());
    BOOST_CHECK_EQUAL(s, desc);

    return make_ready_future();
}

SEASTAR_TEST_CASE(test_bind_callable) {
    uint64_t val;

    auto callable = [&val] { return val; };

    plugin_instance_metrics pm(plugin, my_id, total_bytes("test_bytes", callable));

    auto bound = pm.bound_ids();

    for (auto& id : bound) {
        for (uint64_t i : { 1, 4, 45542, 2323, 12, 0 }) {
            val = i;
            auto vals = get_collectd_value(id);
            for (auto v : vals) {
                BOOST_CHECK_EQUAL(v.u._ui, i);
            }
        }
    }

    return make_ready_future();
}

SEASTAR_TEST_CASE(test_bind_callable_raw) {
    uint64_t val;

    auto callable = [&val] { return val; };

    scollectd::type_instance_id id("apa", per_cpu_plugin_instance, "total_bytes", "ko");
    scollectd::registration r = add_polled_metric(id, make_typed(data_type::DERIVE, callable));

    for (uint64_t i : { 1, 4, 45542, 2323, 12, 0 }) {
        val = i;
        auto vals = get_collectd_value(id);
        for (auto v : vals) {
            BOOST_CHECK_EQUAL(v.u._ui, i);
        }
    }

    return make_ready_future();
}

