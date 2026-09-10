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
 * Copyright (C) 2026 ScyllaDB
 */

// A stand-in for an application built the way Seastar_IMPORT_FMT advertises:
// Seastar's headers, and hence fmt, pulled in as a module rather than
// textually. Seastar's own sources always include fmt textually, so without a
// consumer like this one the `import fmt;` side of
// <seastar/core/internal/fmt.hh> is never compiled at all.
//
// Note the absence of any <fmt/...> include: it is the point of the exercise,
// and mixing one in would be a redefinition error. Everything fmt-related here
// arrives through the funnel. The headers below are the ones that put fmt in
// their interface -- formatters of every kind Seastar writes, compile-time
// format strings -- so that each of those uses is instantiated.

#include <chrono>
#include <vector>

#include <seastar/core/app-template.hh>
#include <seastar/core/format.hh>
#include <seastar/core/future.hh>
#include <seastar/core/sstring.hh>
#include <seastar/net/inet_address.hh>
#include <seastar/net/socket_defs.hh>
#include <seastar/util/log.hh>

using namespace seastar;

static logger applog("hello-import-fmt");

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        // A formatter Seastar defines itself, over a Seastar type.
        applog.info("listening on {}", socket_address(ipv4_addr("127.0.0.1", 1234)));
        // seastar::format(), plus formatters written in terms of
        // fmt::ostream_formatter.
        applog.info("{}", format("address {}, family {}",
                                 net::inet_address("::1"),
                                 net::inet_address::family::INET6));
        // fmt's own formatters for ranges, chrono and strings, as reached
        // through the module.
        applog.info("{} in {}", std::vector<int>{1, 2, 3}, std::chrono::seconds(1));
        applog.info("{:>10}", sstring("right"));
        return make_ready_future<>();
    });
}
