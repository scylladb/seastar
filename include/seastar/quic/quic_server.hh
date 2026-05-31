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
 * Copyright (C) 2026 ScyllaDB Ltd.
 */

#pragma once

#include <memory>
#include <vector>

#include <seastar/core/sstring.hh>
#include <seastar/quic/quic.hh>

namespace seastar::quic::experimental {

// Listener configuration shared by all connections accepted from this server.
struct quic_server_config {
    socket_address listen_address;
    sstring crt_file;
    sstring key_file;
    std::vector<sstring> alpns = {sstring("h3")};
    connection_options session_options{};
};

class quic_server_impl;

// Server-side owner of the listening transport and accepted QUIC connections.
class quic_server final {
public:
    quic_server();
    ~quic_server();

    quic_server(quic_server&&) noexcept;
    quic_server& operator=(quic_server&&) noexcept;

    quic_server(const quic_server&) = delete;
    quic_server& operator=(const quic_server&) = delete;

    future<> start(quic_server_config config);
    future<connection> accept();
    future<> stop();

private:
    std::shared_ptr<quic_server_impl> _impl;
};

} // namespace seastar::quic::experimental
