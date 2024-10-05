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
#include <ranges>

#include <seastar/core/shared_ptr.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/app-template.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/loop.hh>
#include <seastar/net/dns.hh>
#include "tls_echo_server.hh"

using namespace seastar;
namespace bpo = boost::program_options;


int main(int ac, char** av) {
    app_template app;
    app.add_options()
                    ("port", bpo::value<uint16_t>()->default_value(10000), "Remote port")
                    ("address", bpo::value<std::string>()->default_value("127.0.0.1"), "Remote address")
                    ("trust,t", bpo::value<std::string>(), "Trust store")
                    ("msg,m", bpo::value<std::string>(), "Message to send")
                    ("bytes,b", bpo::value<size_t>()->default_value(512), "Use random bytes of length as message")
                    ("iterations,i", bpo::value<size_t>()->default_value(1), "Repeat X times")
                    ("read-response,r", bpo::value<bool>()->default_value(true)->implicit_value(true), "Read echoed message")
                    ("verbose,v", bpo::value<bool>()->default_value(false)->implicit_value(true), "Verbose operation")
                    ("check-name,c", bpo::value<bool>()->default_value(false)->implicit_value(true), "Check server name")
                    ("server-name,s", bpo::value<std::string>(), "Expected server name")
                    ;


    return app.run_deprecated(ac, av, [&] {
        auto&& config = app.configuration();
        uint16_t port = config["port"].as<uint16_t>();
        auto addr = config["address"].as<std::string>();
        auto n = config["bytes"].as<size_t>();
        auto i = config["iterations"].as<size_t>();
        auto do_read = config["read-response"].as<bool>();
        auto verbose = config["verbose"].as<bool>();
        auto check = config["check-name"].as<bool>();

        std::cout << "Starting..." << std::endl;

        auto certs = ::make_shared<tls::certificate_credentials>();
        auto f = make_ready_future();

        if (config.count("trust")) {
            f = certs->set_x509_trust_file(config["trust"].as<std::string>(), tls::x509_crt_format::PEM);
        }

        seastar::shared_ptr<sstring> msg;

        if (config.count("msg")) {
            msg = seastar::make_shared<sstring>(config["msg"].as<std::string>());
        } else {
            msg = seastar::make_shared<sstring>(uninitialized_string(n));
            for (size_t i = 0; i < n; ++i) {
                (*msg)[i] = '0' + char(::rand() % 30);
            }
        }

        sstring server_name;
        if (config.count("server-name")) {
            server_name = config["server-name"].as<std::string>();
        }
        if (verbose) {
            std::cout << "Msg (" << msg->size() << "B):" << std::endl << *msg << std::endl;
        }
        return f.then([=]() {
            return net::dns::get_host_by_name(addr).then([=](net::hostent e) {
                ipv4_addr ia(e.addr_list.front(), port);

                tls::tls_options options;
                if (check) {
                    options.server_name = server_name.empty() ? e.names.front() : server_name;
                }
                return tls::connect(certs, ia, options).then([=](::connected_socket s) {
                    auto strms = ::make_lw_shared<streams>(std::move(s));
                    auto range = std::views::iota(size_t(0), i);
                    return do_for_each(range, [=](auto) {
                        auto f = strms->out.write(*msg);
                        if (!do_read) {
                            return strms->out.close().then([f = std::move(f)]() mutable {
                                return std::move(f);
                            });
                        }
                        return f.then([=]() {
                            return strms->out.flush().then([=] {
                                return strms->in.read_exactly(msg->size()).then([=](temporary_buffer<char> buf) {
                                    sstring tmp(buf.begin(), buf.end());
                                    if (tmp != *msg) {
                                        std::cerr << "Got garbled message!" << std::endl;
                                        if (verbose) {
                                            std::cout << "Got (" << tmp.size() << ") :" << std::endl << tmp << std::endl;
                                        }
                                        throw std::runtime_error("Got garbled message!");
                                    }
                                });
                            });
                        });
                    }).then([strms, do_read]{
                        return do_read ? strms->out.close() : make_ready_future<>();
                    }).finally([strms]{
                        return strms->in.close();
                    });
                });
            }).handle_exception([](auto ep) {
                std::cerr << "Error: " << ep << std::endl;
            });
        }).finally([] {
            engine().exit(0);
        });
    });
}
