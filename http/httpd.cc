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
 * Copyright 2015 Cloudius Systems
 */

#include "core/reactor.hh"
#include "core/sstring.hh"
#include "core/app-template.hh"
#include "core/circular_buffer.hh"
#include "core/distributed.hh"
#include "core/queue.hh"
#include "core/future-util.hh"
#include "core/metrics.hh"
#include <iostream>
#include <algorithm>
#include <unordered_map>
#include <queue>
#include <bitset>
#include <limits>
#include <cctype>
#include <vector>
#include "httpd.hh"

using namespace std::chrono_literals;

namespace httpd {
http_stats::http_stats(http_server& server)
 {
    namespace sm = seastar::metrics;
    _metric_groups.add_group("httpd", {
            sm::make_derive("connections_total", [&server] { return server.total_connections(); }, sm::description("The total number of connections opened")),
            sm::make_gauge("connections_current", [&server] { return server.current_connections(); }, sm::description("The current number of open  connections")),
            sm::make_derive("requests_served", [&server] { return server.requests_served(); }, sm::description("The total number of http requests served"))
    });
}
}
