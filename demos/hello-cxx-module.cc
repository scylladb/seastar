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
 * Copyright (C) 2023 ScyllaDB Ltd.
 */

#include <exception>
#include <stdexcept>

import seastar;

using namespace seastar;
logger applog("app");

int main(int argc, char** argv) {
    seastar::app_template app;
    app.run(argc, argv, [] () -> future<> {
        applog.info("Hello world!");
        // A compatibility check, as CI builds this demo with the C++ module:
        // formatting a std::exception_ptr directly is deprecated (prefer
        // seastar::formattable()), but applications still do it, and it must
        // keep compiling through the module too (#3831).
        applog.info("An exception: {}", std::make_exception_ptr(std::runtime_error("test")));
        return make_ready_future<>();
    });
}
