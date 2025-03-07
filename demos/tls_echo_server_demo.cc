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
#include <cmath>
#include <seastar/core/reactor.hh>
#include <seastar/core/app-template.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/thread.hh>
#include <seastar/net/dns.hh>
#include <seastar/util/closeable.hh>
#include "../apps/lib/stop_signal.hh"
#include "tls_echo_server.hh"

using namespace seastar;
namespace bpo = boost::program_options;


int main(int ac, char** av) {
    app_template app;
    app.add_options()
                    ("port", bpo::value<uint16_t>()->default_value(10000), "Server port")
                    ("address", bpo::value<std::string>()->default_value("127.0.0.1"), "Server address")
                    ("cert,c", bpo::value<std::string>()->required(), "Server certificate file")
                    ("key,k", bpo::value<std::string>()->required(), "Certificate key")
                    ("verbose,v", bpo::value<bool>()->default_value(false)->implicit_value(true), "Verbose")
                    ;
    return app.run(ac, av, [&app] {
        return async([&app] {
            seastar_apps_lib::stop_signal stop_signal;
            auto&& config = app.configuration();
            uint16_t port = config["port"].as<uint16_t>();
            auto crt = config["cert"].as<std::string>();
            auto key = config["key"].as<std::string>();
            auto addr = config["address"].as<std::string>();
            auto verbose = config["verbose"].as<bool>();

            std::cout << "Starting..." << std::endl;
            net::inet_address a = net::dns::resolve_name(addr).get();

            ipv4_addr ia(a, port);

            seastar::sharded<echoserver> server;
            server.start(verbose).get();
            auto stop_server = deferred_stop(server);

            try {
                server.invoke_on_all(&echoserver::listen, socket_address(ia), sstring(crt), sstring(key), tls::client_auth::NONE).get();
            } catch (...) {
                std::cerr << "Error: " << std::current_exception() << std::endl;
                return 1;
            }
            std::cout << "TLS echo server running at " << addr << ":" << port << std::endl;
            stop_signal.wait().get();
            return 0;
        });
    });
}
