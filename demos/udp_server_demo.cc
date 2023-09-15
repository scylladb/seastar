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


#include <seastar/core/reactor.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/when_all.hh>
#include <seastar/core/distributed.hh>
#include <seastar/core/app-template.hh>
#include <seastar/core/gate.hh>
#include <seastar/core/smp.hh>
#include <seastar/core/sleep.hh>
#include "../apps/lib/stop_signal.hh"
#include <iostream>


using namespace std::chrono_literals;

class udp_server {
private:
    seastar::net::udp_channel _chan;
    seastar::timer<> _stats_timer;
    uint64_t _n_sent {};
    seastar::gate _gate;
    std::unique_ptr<seastar::future<>> _stopper;
public:
    void start(uint16_t port) {
        seastar::ipv4_addr listen_addr{port};
        _chan = seastar::make_udp_channel(listen_addr);

        _stats_timer.set_callback([this] {
            std::cout << "Out: " << _n_sent << " pps" << std::endl;
            _n_sent = 0;
        });
        _stats_timer.arm_periodic(1s);

        // Run server in background.
        seastar::future<> stopper = seastar::async([this]() {
            while(true) {
                 try {
                    seastar::with_gate(_gate, [this] {
                        seastar::net::udp_datagram dgram = _chan.receive().get();
                        _chan.send(dgram.get_src(), std::move(dgram.get_data())).get();
                        _n_sent++;
                    }).get();
                } catch (seastar::gate_closed_exception&) {
                    std::cout << "Gate closed" << std::endl;
                    break;
                } catch (seastar::broken_promise& e) {
                    // Note - in case the other end closed the session this can happen.
                    std::cerr << e.what() << std::endl;
                }
            }

            if (!_chan.is_closed())
                _chan.close();
            _stats_timer.cancel();
        });

        _stopper = std::make_unique<seastar::future<>>(std::move(stopper));
    }

    seastar::future<> stop() {
        // Note that we need to call `_chan.close` here in case we are not getting any packets.
        // In such a case the receiver will just hang and won't be terminated.
        // By actively closing the channel here we will trigger `broken_promise` on the receiver.
        _chan.close();

        // Note that it is not enough to just close the gate here.
        // If we just wait on `_gate.close` and ignore the stopper we will have a race on our hands.
        // If the receiver thread is not currently within the gate (i.e gate.close will return) it doesn't mean
        // it was terminated. If we return from the stop call before that we reach a scenario in which the 
        // object is destroyed but the thread wasn't terminated yet -> use_after_free issue.
        return seastar::when_all(_gate.close(), std::move(*_stopper.get())).discard_result();
    }
};

namespace bpo = boost::program_options;

int main(int ac, char ** av) {
    seastar::app_template app;
    
    app.add_options()
        ("port", bpo::value<uint16_t>()->default_value(10000), "UDP server port") ;
    return app.run(ac, av, [&]() {
        return seastar::async([&]{
            seastar_apps_lib::stop_signal stop_signal;
            auto&& config = app.configuration();
            uint16_t port = config["port"].as<uint16_t>();
            auto server = std::make_unique<seastar::distributed<udp_server>>();

            // Run server in background.
            server->start().get();

            std::cout << "Start listening on all shards...\n";
            server->invoke_on_all(&udp_server::start, port).get();

            std::cout << "Seastar UDP server listening on port " << port << " ...\n";
            stop_signal.wait().get();

            std::cout << "Application going down..." << std::endl;
            server->stop().get();

            std::cout << "Termination finished." << std::endl;
        });
    });
}