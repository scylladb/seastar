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
 * Copyright 2017 ScyllaDB
 */
#pragma once

#include <seastar/core/sstring.hh>
#include <seastar/core/do_with.hh>
#include <seastar/core/sharded.hh>
#include <seastar/core/gate.hh>
#include <seastar/core/loop.hh>
#include <seastar/net/tls.hh>
#include <seastar/util/log.hh>
#include <iostream>

using namespace seastar;

struct streams {
    connected_socket s;
    input_stream<char> in;
    output_stream<char> out;

    streams(connected_socket cs) : s(std::move(cs)), in(s.input()), out(s.output())
    {}
};

class echoserver {
    server_socket _socket;
    shared_ptr<tls::server_credentials> _certs;
    seastar::gate _gate;
    bool _stopped = false;
    bool _verbose = false;
public:
    echoserver(bool verbose = false)
            : _certs(make_shared<tls::server_credentials>(make_shared<tls::dh_params>()))
            , _verbose(verbose)
    {}

    future<> listen(socket_address addr, sstring crtfile, sstring keyfile, tls::client_auth ca = tls::client_auth::NONE) {
        _certs->set_client_auth(ca);
        return _certs->set_x509_key_file(crtfile, keyfile, tls::x509_crt_format::PEM).then([this, addr] {
            ::listen_options opts;
            opts.reuse_address = true;

            _socket = tls::listen(_certs, addr, opts);

            // Listen in background.
            (void)repeat([this] {
                if (_stopped) {
                    return make_ready_future<stop_iteration>(stop_iteration::yes);
                }
                return with_gate(_gate, [this] {
                    return _socket.accept().then([this](accept_result ar) {
                        ::connected_socket s = std::move(ar.connection);
                        socket_address a = std::move(ar.remote_address);
                        if (_verbose) {
                            std::cout << "Got connection from "<< a << std::endl;
                        }
                        auto strms = make_lw_shared<streams>(std::move(s));
                        return repeat([strms, this]() {
                            return strms->in.read().then([this, strms](temporary_buffer<char> buf) {
                                if (buf.empty()) {
                                    if (_verbose) {
                                        std::cout << "EOM" << std::endl;
                                    }
                                    return make_ready_future<stop_iteration>(stop_iteration::yes);
                                }
                                sstring tmp(buf.begin(), buf.end());
                                if (_verbose) {
                                    std::cout << "Read " << tmp.size() << "B" << std::endl;
                                }
                                return strms->out.write(tmp).then([strms]() {
                                    return strms->out.flush();
                                }).then([] {
                                    return make_ready_future<stop_iteration>(stop_iteration::no);
                                });
                            });
                        }).then([strms]{
                            return strms->out.close();
                        }).handle_exception([](auto ep) {
                        }).finally([this, strms]{
                            if (_verbose) {
                                std::cout << "Ending session" << std::endl;
                            }
                            return strms->in.close();
                        });
                    }).handle_exception([this](auto ep) {
                        if (!_stopped) {
                            std::cerr << "Error: " << ep << std::endl;
                        }
                    }).then([this] {
                        return make_ready_future<stop_iteration>(_stopped ? stop_iteration::yes : stop_iteration::no);
                    });
                });
            });
            return make_ready_future();
        });
    }

    future<> stop() {
        _stopped = true;
        _socket.abort_accept();
        return _gate.close();
    }
};
