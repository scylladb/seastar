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

#pragma once

#include <seastar/core/seastar.hh>
#include <seastar/net/api.hh>
#include <seastar/net/tls.hh>

namespace seastar::http::experimental {

/**
 * \brief Factory that provides transport for \ref client
 *
 * This customization point allows callers provide its own transport for client. The
 * client code calls factory when it needs more connections to the server.
 */

class connection_factory {
public:
    /**
     * \brief Make a \ref connected_socket
     *
     * The implementations of this method should return ready-to-use socket that will
     * be used by \ref client as transport for its http connections
     */
    virtual future<connected_socket> make(abort_source*) = 0;
    virtual ~connection_factory() {}
};

class basic_connection_factory : public connection_factory {
    socket_address _addr;
public:
    explicit basic_connection_factory(socket_address addr)
        : _addr(std::move(addr))
    {
    }
    virtual future<connected_socket> make(abort_source* as) override {
        return seastar::connect(_addr, {}, transport::TCP);
    }
};

class tls_connection_factory : public connection_factory {
    socket_address _addr;
    shared_ptr<tls::certificate_credentials> _creds;
    sstring _host;
public:
    tls_connection_factory(socket_address addr, shared_ptr<tls::certificate_credentials> creds, sstring host)
        : _addr(std::move(addr))
        , _creds(std::move(creds))
        , _host(std::move(host))
    {
    }
    virtual future<connected_socket> make(abort_source* as) override {
        return tls::connect(_creds, _addr, tls::tls_options{.server_name = _host});
    }
};

}
