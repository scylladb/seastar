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
#include <optional>
#include <string_view>
#include <vector>

struct prometheus_test_fixture;

namespace seastar {

namespace prometheus {

/*!
 * \brief A content coding that the reply to a metrics request can be compressed with
 *
 * A coding is used only if it is listed in config::compression and the client
 * accepts it in its Accept-Encoding header.
 * Seastar supports a coding only if it was built with the library implementing it.
 */
enum class compression_type {
    gzip, //!< gzip, requires zlib
    zstd, //!< Zstandard, requires libzstd
};

/*!
 * \brief Parses a comma separated list of content coding names
 *
 * The names are those used in the Content-Encoding HTTP header: "gzip" and "zstd".
 * An empty string, or "none", stands for no compression and returns an empty list.
 *
 * It is a helper for applications that take config::compression from their
 * own configuration.
 *
 * \param names comma separated coding names, for example "gzip,zstd"
 * \return the codings, in the order they were named
 * \throws std::invalid_argument if a name is not that of a known coding
 */
std::vector<compression_type> parse_compression(std::string_view names);


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
    /*!
     * \brief The content codings that a reply may be compressed with
     *
     * It is empty by default, which disables compression.
     * A reply is compressed with one of these codings if the request's
     * Accept-Encoding header accepts it. If it accepts more than one of them,
     * zstd is preferred to gzip, regardless of the order here or in the request.
     *
     * Compression is done on the reactor thread, at the fastest level of the library.
     *
     * Registering the routes fails if Seastar was built without the library
     * that a listed coding requires.
     */
    std::vector<compression_type> compression;
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
};

// Creates a family filter from multiple name filters.
// Returns true if the family name matches any of the filters.
// If filters is empty, returns a filter that matches all families.
// If prefix is provided, filter names starting with "{prefix}_" will have the prefix stripped,
// allowing users to query with either "foo" or "seastar_foo" when prefix is "seastar".
family_filter_t make_family_filter(std::vector<name_filter> filters, std::string_view prefix = "");

// Chooses the content coding of a reply.
// Returns the preferred coding among those that are both in enabled and accepted
// by the value of the request's Accept-Encoding header, if there is one.
std::optional<compression_type> select_compression(std::string_view accept_encoding, const std::vector<compression_type>& enabled);

struct write_body_args {
    filter_t filter;
    family_filter_t family_filter;
    bool use_protobuf_format;
    bool show_help;
    bool enable_aggregation;
    std::optional<compression_type> compression = std::nullopt; // compress the body with this coding
};

class test_access {
    future<> write_body(config cfg, write_body_args args, output_stream<char>&& s);

    friend struct metrics_perf_fixture;
    friend struct ::prometheus_test_fixture;
};
}
}
}
