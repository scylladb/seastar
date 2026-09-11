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

// The application half of the installed-package test; see CMakeLists.txt in
// this directory for what is being tested and how to run it.
//
// It stays deliberately small. What is under test is the packaging -- headers
// found, flags and definitions arriving, every dependency named, the whole
// thing linking -- not Seastar's behaviour, which the unit tests cover.
//
// It does reach for pieces that live in the library rather than the headers,
// since an application that resolves nothing is not much of a link test: a
// logger call (logger::failed_to_log), a formatter defined out of line
// (log_level), and one built on fmt::ostream_formatter (socket_address).

#include <seastar/core/app-template.hh>
#include <seastar/core/format.hh>
#include <seastar/core/future.hh>
#include <seastar/net/socket_defs.hh>
#include <seastar/util/log.hh>

using namespace seastar;

static logger applog("consumer");

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        applog.info("hello from {}, at level {}",
                    socket_address(ipv4_addr("127.0.0.1", 1234)),
                    log_level::info);
        applog.info("{}", format("{:>8}", "and format()"));
        return make_ready_future<>();
    });
}
