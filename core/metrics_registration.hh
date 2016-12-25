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

#pragma once

/*!
 * \file metrics_registration.hh
 * \brief holds the metric_groups definition needed by class that reports metrics
 *
 * If class A needs to report metrics,
 * typically you include metrics_registration.hh, in A header file and add to A:
 * * metric_groups _metrics as a member
 * * set_metrics() method that would be called in the constructor.
 * \code
 * class A {
 *   metric_groups _metrics
 *
 *   void setup_metrics();
 *
 * };
 * \endcode
 * To define the metrics, include in your source file metircs.hh
 * @see metrics.hh for the definition for adding a metric.
 */

namespace seastar {

namespace metrics {

namespace impl {
class metric_groups_def;
struct metric_definition_impl;
class metric_groups_impl;
}

using group_name_type = sstring; /*!< A group of logically related metrics */
class metric_groups;

class metric_definition {
    std::unique_ptr<impl::metric_definition_impl> _impl;
public:
    metric_definition(const impl::metric_definition_impl& impl);
    metric_definition(metric_definition&& m) : _impl(std::move(m._impl)) {
    }
    friend metric_groups;
    friend impl::metric_groups_impl;
};

/*!
 * metric_groups
 * \brief holds the metric definition.
 *
 * Use the add_group method to add a group of metrics @see metrics.hh for example and supported metrics
 */
class metric_groups {
    std::unique_ptr<impl::metric_groups_def> _impl;
public:
    metric_groups();
    /*!
     * \brief add metrics belong to the same group.
     *
     * use the metrics creation functions to add metrics.
     *
     * for example:
     *  _metrics.add_group("my_group", {
     *      make_counter("my_counter_name1", counter, description("my counter description")),
     *      make_counter("my_counter_name2", counter, description("my second counter description")),
     *      make_gauge("my_gauge_name1", gauge, description("my gauge description")),
     *  });
     *
     *  metric name should be unique inside the group.
     *  you can change add_group calls like:
     *  _metrics.add_group("my group1", {...}).add_group("my group2", {...});
     */
    metric_groups& add_group(const group_name_type& name, const std::initializer_list<metric_definition>& l);
};


}
}
