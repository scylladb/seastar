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
#include <seastar/core/internal/api-level.hh>
#include <chrono>
#include <optional>
#include <string_view>
#include <vector>

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
using family_filter_t = std::function<bool(std::string_view)>;

struct name_filter {
    sstring name;
    bool is_prefix;

    bool operator==(const name_filter&) const = default;
};

// Creates a family filter from multiple name filters.
// Returns true if the family name matches any of the filters.
// If filters is empty, returns a filter that matches all families.
// If prefix is provided, filter names starting with "{prefix}_" will have the prefix stripped,
// allowing users to query with either "foo" or "seastar_foo" when prefix is "seastar".
family_filter_t make_family_filter(std::vector<name_filter> filters, std::string_view prefix = "");

// Identifies the filters of a request, as specified by its parameters
struct filter_key {
    std::vector<name_filter> names;
    // Label matchers: labels and regular expressions, sorted by label
    std::vector<std::pair<sstring, sstring>> labels;

    bool operator==(const filter_key&) const = default;
};

struct write_body_args {
    filter_t filter;
    family_filter_t family_filter;
    bool use_protobuf_format;
    bool show_help;
    bool enable_aggregation;
    // Identifies filter and family_filter: requests with equal keys must
    // have equivalent filters. If set, the text representation's template
    // is cached.
    std::optional<filter_key> cache_key = std::nullopt;
};

// Statistics of the text representation template cache of a shard
struct text_cache_stats {
    uint64_t hits = 0;          // requests served from a cached template
    uint64_t builds = 0;        // templates built
    uint64_t invalidations = 0; // cached templates which didn't fit the metrics
    size_t entries = 0;         // templates in the cache
};

class test_access {
    future<> write_body(config cfg, write_body_args args, output_stream<char>&& s);
    // Returns the template cache statistics of this shard
    static text_cache_stats cache_stats();
    // Clears the template cache and its statistics on this shard
    static void clear_cache();
    // Sets the time after which templates expire, on all shards
    static void set_cache_ttl(std::chrono::milliseconds ttl);
    // Returns a template cached by this shard, or nullptr
    static const void* cached_template();
    // Overrides the shards' NUMA nodes when sharing templates, on all shards;
    // must not be called while requests are served.
    static void set_numa_node_mapping(std::optional<std::vector<unsigned>> mapping);

    friend struct metrics_perf_fixture;
    friend struct ::prometheus_test_fixture;
};
}
}
}
