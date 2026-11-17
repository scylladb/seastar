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

/// \brief Multi-shard server entry points for the experimental QUIC transport.
namespace seastar::quic::experimental {

/// \brief Per-shard QUIC listener using UDP SO_REUSEPORT when the POSIX stack supports it.
///
/// The class owns one quic_server instance per shard. Packets are normally
/// distributed by the kernel receive path. Server-generated connection IDs encode
/// the owning shard, so packets for established connections that arrive on a
/// non-owner shard are authenticated and forwarded to the owning shard.
///
/// The object itself is controlled from one shard. Each connection and its
/// handler remain on the shard that owns the connection.
class sharded_quic_server final {
public:
    /// \brief Per-connection coroutine executed on the connection's owning shard.
    using accept_handler = noncopyable_function<future<> (connection)>;

    /// \brief Factory invoked once on every shard to create its local accept handler.
    using accept_handler_factory = std::function<accept_handler ()>;

    /// \brief Construct a stopped sharded server.
    sharded_quic_server();

    /// \brief Destroy the controller and request detached cleanup if it is still running.
    ~sharded_quic_server();

    /// \brief Sharded servers cannot be moved.
    sharded_quic_server(sharded_quic_server&&) noexcept = delete;

    /// \brief Sharded servers cannot be move-assigned.
    sharded_quic_server& operator=(sharded_quic_server&&) noexcept = delete;

    /// \brief Sharded servers cannot be copied.
    sharded_quic_server(const sharded_quic_server&) = delete;

    /// \brief Sharded servers cannot be copy-assigned.
    sharded_quic_server& operator=(const sharded_quic_server&) = delete;

    /// \brief Start one UDP listener on every shard.
    ///
    /// All listeners bind the same endpoint with `SO_REUSEPORT`. If the
    /// configured port is zero, one ephemeral port is selected and reused by
    /// every shard.
    ///
    /// \param config Shared listener, TLS, ALPN, and connection configuration.
    /// \return A future that fails with quic_error if already started, the
    ///         configuration is invalid, or the active network stack cannot
    ///         provide a multi-shard reuse-port listener.
    future<> start(quic_server_config config);

    /// \brief Start an accept loop on every shard.
    ///
    /// Each accepted connection is passed to the handler on its owning shard.
    /// Multiple handler invocations on a shard may overlap. If creating a
    /// handler or an accept loop fails, the whole sharded server is stopped.
    ///
    /// \param make_handler Factory evaluated once on each shard.
    /// \return A future that fails with quic_error if start() has not completed
    ///         or serve() is already active.
    future<> serve(accept_handler_factory make_handler);

    /// \brief Stop accept loops, listeners, and connections on every shard.
    ///
    /// The operation waits for connection handlers and background work to finish
    /// and is idempotent.
    future<> stop();

    /// \brief Get the common UDP endpoint used by all shards.
    ///
    /// \return The effective endpoint, including an automatically selected port,
    ///         or an empty address before start() and after stop().
    socket_address local_address() const noexcept;

private:
    class impl;
    std::unique_ptr<impl> _impl;
};

} // namespace seastar::quic::experimental
