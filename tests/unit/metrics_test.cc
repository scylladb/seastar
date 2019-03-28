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
#include <seastar/testing/test_case.hh>

SEASTAR_TEST_CASE(test_add_group) {
    using namespace seastar::metrics;
    // Just has to compile:
    metric_groups()
        .add_group("g1", {})
        .add_group("g2", std::vector<metric_definition>());
    return seastar::make_ready_future();
}
