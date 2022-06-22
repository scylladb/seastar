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
 * Copyright (C) 2021 ScyllaDB Ltd.
 */

#include <iostream>
#include <seastar/websocket/server.hh>
#include <seastar/core/app-template.hh>
#include <seastar/core/fstream.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/loop.hh>
#include <seastar/core/thread.hh>
#include <seastar/util/defer.hh>

using namespace seastar;
using namespace seastar::experimental;

int main(int argc, char** argv) {
    seastar::app_template app;
    app.run(argc, argv, [] () -> seastar::future<> {
        return async([] {
            static websocket::server ws;
            ws.register_handler("echo", [] (input_stream<char>& in,
                        output_stream<char>& out) {
                return repeat([&in, &out]() {
                    return in.read().then([&out](temporary_buffer<char> f) {
                        std::cerr << "f.size(): " << f.size() << "\n";
                        if (f.empty()) {
                            return make_ready_future<stop_iteration>(stop_iteration::yes);
                        } else {
                            return out.write(std::move(f)).then([&out]() {
                                return out.flush().then([] {
                                    return make_ready_future<stop_iteration>(stop_iteration::no);
                                });
                            });
                        }
                    });
                });
            });
            auto d = defer([] () noexcept {
                ws.stop().get();
            });
            ws.listen(socket_address(ipv4_addr("127.0.0.1", 8123)));
            std::cout << "Listening on 127.0.0.1:8123 for 1 hour (interruptible, hit Ctrl-C to stop)..." << std::endl;
            seastar::sleep_abortable(std::chrono::hours(1)).get();
            std::cout << "Stopping the server, deepest thanks to all clients, hope we meet again" << std::endl;
        });
    });
}
