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

#include <functional>
#include <memory>

#include <seastar/core/future.hh>
#include <seastar/net/api.hh>
#include <seastar/quic/quic_server.hh>
#include <seastar/util/noncopyable_function.hh>

namespace seastar::quic::experimental {

/// Per-shard QUIC listener using UDP SO_REUSEPORT when the POSIX stack supports it.
///
/// The class owns one quic_server instance per shard. Packets are normally
/// distributed by the kernel receive path. Server-generated connection IDs encode
/// the owning shard, so packets for established connections that arrive on a
/// non-owner shard can be forwarded to the shard that owns the connection.
class sharded_quic_server final {
public:
    using accept_handler = noncopyable_function<future<> (connection)>;
    using accept_handler_factory = std::function<accept_handler ()>;

    sharded_quic_server();
    ~sharded_quic_server();

    sharded_quic_server(sharded_quic_server&&) noexcept = delete;
    sharded_quic_server& operator=(sharded_quic_server&&) noexcept = delete;

    sharded_quic_server(const sharded_quic_server&) = delete;
    sharded_quic_server& operator=(const sharded_quic_server&) = delete;

    /// Starts one listener on each shard.
    future<> start(quic_server_config config);

    /// Starts per-shard accept loops. The factory is evaluated once on each shard.
    future<> serve(accept_handler_factory make_handler);

    /// Stops accept loops, listeners, and server-owned connections on every shard.
    future<> stop();

    /// Returns the local UDP endpoint selected during start().
    socket_address local_address() const noexcept;

private:
    class impl;
    std::unique_ptr<impl> _impl;
};

} // namespace seastar::quic::experimental
