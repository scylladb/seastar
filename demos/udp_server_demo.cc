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
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 */

#include <iostream>
#include <seastar/core/app-template.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/sharded.hh>
#include <seastar/core/thread.hh>
#include <seastar/util/closeable.hh>
#include "../apps/lib/stop_signal.hh"

using namespace seastar;
using namespace net;
using namespace std::chrono_literals;

class udp_server {
private:
    std::optional<udp_channel> _chan;
    std::optional<future<>> _task;
    timer<> _stats_timer;
    uint64_t _n_sent {};
public:
    void start(uint16_t port) {
        ipv4_addr listen_addr{port};
        _chan = make_bound_datagram_channel(listen_addr);

        _stats_timer.set_callback([this] {
            std::cout << "Out: " << _n_sent << " pps" << std::endl;
            _n_sent = 0;
        });
        _stats_timer.arm_periodic(1s);

        // Run server in background.
        _task = keep_doing([this] {
            return _chan->receive().then([this] (datagram dgram) {
                auto bufs = std::move(dgram.get_data()).release();
                return _chan->send(dgram.get_src(), bufs).then([this] {
                    _n_sent++;
                });
            });
        });
    }
    future<> stop() {
        if (_chan) {
            _chan->shutdown_input();
            _chan->shutdown_output();
        }
        if (_task) {
            co_await _task->handle_exception([](std::exception_ptr e) {
                std::cerr << "exception in udp_server: " << e << "\n";
            });
        }
    }
};

namespace bpo = boost::program_options;

int main(int ac, char ** av) {
    app_template app;
    app.add_options()
        ("port", bpo::value<uint16_t>()->default_value(10000), "UDP server port") ;
   return app.run(ac, av, [&] {
        return async([&app] {
            seastar_apps_lib::stop_signal stop_signal;
            auto&& config = app.configuration();
            uint16_t port = config["port"].as<uint16_t>();
            sharded<udp_server> server;
            if (engine().net().has_per_core_namespace()) {
                server.start().get();
            } else {
                server.start_single().get();
            }
            auto stop_server = deferred_stop(server);
            server.invoke_on_all(&udp_server::start, port).get();
            std::cout << "Seastar UDP server listening on port " << port << " ...\n";

            stop_signal.wait().get();
        });
    });
}
