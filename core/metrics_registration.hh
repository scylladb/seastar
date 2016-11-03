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
}


/*!
 * metric_groups
 * \brief holds the metric definition.
 *
 * Use the add_group method to add a group of metrics @see metrics.hh for example and supported metrics
 */
using metric_groups = std::unique_ptr<impl::metric_groups_def>;

/*!
 * \brief create a metric_groups variable
 *
 * Use this function to initialize the metric_group.
 * This is typically done as a first thing in the method that create the metrics.
 */
metric_groups create_metric_groups();


}
}
