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
 * Copyright (C) 2016 ScyllaDB
 */

#pragma once

#include <seastar/http/httpd.hh>
#include <seastar/core/metrics.hh>
#include <seastar/util/std-compat.hh>
#include <optional>

namespace seastar {

namespace prometheus {

/*!
 * Holds prometheus related configuration
 */
struct config {
    sstring metric_help; //!< Default help message for the returned metrics
    sstring hostname; //!< hostname is deprecated, use label instead
    std::optional<metrics::label_instance> label; //!< A label that will be added to all metrics, we advice not to use it and set it on the prometheus server
    sstring prefix = "seastar"; //!< a prefix that will be added to metric names
};

future<> start(httpd::http_server_control& http_server, config ctx);

/// \defgroup add_prometheus_routes adds a /metrics endpoint that returns prometheus metrics
///    in txt format
/// @{
future<> add_prometheus_routes(distributed<http_server>& server, config ctx);
future<> add_prometheus_routes(http_server& server, config ctx);
/// @}
}
}
