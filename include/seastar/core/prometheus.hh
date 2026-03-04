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
#include <seastar/core/metrics_api.hh>
#include <seastar/util/std-compat.hh>
#include <optional>

struct prometheus_test_fixture;

namespace seastar {

namespace prometheus {


/*!
 * Holds prometheus related configuration
 */
struct config {

    [[deprecated("metric_help is deprecated and no longer used, to be removed in 2027")]]
    sstring metric_help;

    [[deprecated("hostname is deprecated and unused, use label instead, to be removed in 2027")]]
    sstring hostname;

    SEASTAR_INTERNAL_BEGIN_IGNORE_DEPRECATIONS // prevent warnings about deprecated fields in implicitly-defined special member functions
    config() = default;
    config(const config&) = default;
    config(config&&) = default;
    ~config() = default;
    SEASTAR_INTERNAL_END_IGNORE_DEPRECATIONS

    std::optional<metrics::label_instance> label; //!< A label that will be added to all metrics, we advice not to use it and set it on the prometheus server
    sstring prefix = "seastar"; //!< a prefix that will be added to metric names
    bool allow_protobuf = false; // protobuf support is experimental and off by default
};

future<> start(httpd::http_server_control& http_server, config ctx);

/// \defgroup add_prometheus_routes adds a /metrics endpoint that returns prometheus metrics
///    both in txt format and in protobuf according to the prometheus spec
/// @{
future<> add_prometheus_routes(sharded<httpd::http_server>& server, config ctx);
future<> add_prometheus_routes(httpd::http_server& server, config ctx);
/// @}

namespace details {
using filter_t = std::function<bool(const metrics::impl::labels_type&)>;

class test_access {
    future<> write_body(config cfg, bool use_protobuf_format, sstring metric_family_name, bool prefix, bool show_help, bool enable_aggregation, filter_t filter, output_stream<char>&& s);

    friend struct metrics_perf_fixture;
    friend struct ::prometheus_test_fixture;
};
}
}
}
