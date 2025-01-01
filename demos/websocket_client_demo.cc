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

#include <iostream>
#include <seastar/net/dns.hh>
#include <seastar/websocket/client.hh>
#include <seastar/core/app-template.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/when_all.hh>

using namespace seastar;
using namespace seastar::experimental;

namespace bpo = boost::program_options;

int main(int argc, char** argv) {
    seastar::app_template app;
    app.add_options()
        ("host", bpo::value<std::string>(), "Host to connect")
        ("port", bpo::value<std::uint16_t>(), "Port to connect")
        ("path", bpo::value<std::string>(), "Path to query upon")
        ("subprotocol", bpo::value<std::string>()->default_value(""), "Sub-protocol")
        ;
    app.run(argc, argv, [&app]() -> seastar::future<> {
        auto&& config = app.configuration();
        auto host = config["host"].as<std::string>();
        auto port = config["port"].as<std::uint16_t>();
        auto path = config["path"].as<std::string>();
        auto subprotocol = config["subprotocol"].as<std::string>();

        return async([=] {
            net::hostent e = net::dns::get_host_by_name(host, net::inet_address::family::INET).get();
            auto ws = std::make_unique<websocket::client>(socket_address(e.addr_list.front(), port));

            if (!subprotocol.empty()) {
                ws->set_subprotocol(subprotocol);
            }

            auto req = http::request::make("GET", host, path);

            auto handler = [](input_stream<char>& in,
                              output_stream<char>& out) {
                return repeat([&in, &out]() {
                    return in.read().then([&out](temporary_buffer<char> f) {
                        auto value = std::stol(std::string(f.get(), f.size()));
                        std::cout << "got " << value << "\n";
                        auto new_str = std::to_string(value + 1);
                        return out.write(temporary_buffer<char>(new_str.data(), new_str.size()))
                            .then([&out] { return out.flush(); })
                            .then([] {
                                return make_ready_future<stop_iteration>(stop_iteration::no);
                            });
                    });
                });
            };

            std::cout << "Sending messages to " << host << ":" << port
                      << " for 1 hour (interruptible, hit Ctrl-C to stop)..." << std::endl;

            seastar::shared_ptr<websocket::client_connection> client_con;

            ws->make_request(std::move(req), handler).then(
                        [&ws, &client_con](auto con) -> future<> {
                client_con = con;
                return when_all_succeed(
                    [con]{ return con->process(); },
                    [con]{ return con->send_message(temporary_buffer<char>("1", 1), true); },
                    [&ws]{
                        return sleep_abortable(std::chrono::hours(1))
                                .handle_exception([&ws](auto ignored) {
                            std::cout << "Stopping the client" << std::endl;
                            return ws->stop();
                        });
                    }
                ).discard_result();
            }).get();
        });
    });
}
