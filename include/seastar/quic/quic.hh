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

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

#include <seastar/core/future.hh>
#include <seastar/core/iostream.hh>
#include <seastar/core/sstring.hh>
#include <seastar/core/temporary_buffer.hh>
#include <seastar/net/api.hh>
#include <seastar/quic/quic_error.hh>

namespace seastar::quic::experimental {

/// QUIC stream identifier as defined by RFC 9000.
using stream_id = int64_t;

/// Application-defined QUIC stream or connection error code.
using application_error_code = uint64_t;

/// Sentinel used internally before ngtcp2 assigns a real stream id.
inline constexpr stream_id invalid_stream_id = -1;

/// Directionality of a QUIC stream.
enum class stream_type : uint8_t {
    bidirectional,
    unidirectional,
};

/// Congestion controller selected for new QUIC connections.
enum class congestion_control_algorithm : uint8_t {
    reno,
    cubic,
    bbr,
    bbr2,
};

/// Transport-level knobs passed to ngtcp2 when a connection is created.
///
/// The fields mirror ngtcp2 transport settings and parameters. See the ngtcp2
/// documentation for protocol-level details of each window, payload and PMTUD
/// option.
struct transport_config {
    /// Maximum idle time before the peer may close the connection.
    std::chrono::nanoseconds max_idle_timeout = std::chrono::seconds{60};

    /// Initial receive window for locally-initiated bidirectional streams.
    uint64_t initial_max_stream_data_bidi_local = 256 * 1024;

    /// Initial receive window for remotely-initiated bidirectional streams.
    uint64_t initial_max_stream_data_bidi_remote = 256 * 1024;

    /// Initial receive window for unidirectional streams.
    uint64_t initial_max_stream_data_uni = 256 * 1024;

    /// Initial connection-wide receive window.
    uint64_t initial_max_data = 4 * 1024 * 1024;

    /// Maximum number of concurrent bidirectional streams the peer may open.
    uint64_t initial_max_streams_bidi = 128;

    /// Maximum number of concurrent unidirectional streams the peer may open.
    uint64_t initial_max_streams_uni = 128;

    /// Maximum UDP payload size this endpoint will transmit.
    std::optional<size_t> max_tx_udp_payload_size{};

    /// Maximum UDP payload size this endpoint advertises to the peer.
    std::optional<uint64_t> max_udp_payload_size{};

    /// Initial RTT estimate used before path validation samples are available.
    std::optional<std::chrono::nanoseconds> initial_rtt{};

    /// Connection-wide auto-tuned flow control window cap.
    std::optional<uint64_t> max_window{};

    /// Per-stream auto-tuned flow control window cap.
    std::optional<uint64_t> max_stream_window{};

    /// ACK threshold passed to ngtcp2.
    std::optional<size_t> ack_thresh{};

    /// Congestion controller override.
    std::optional<congestion_control_algorithm> congestion_control{};

    /// Disables ngtcp2 transmit UDP payload size shaping.
    bool disable_tx_udp_payload_size_shaping = false;

    /// Disables path MTU discovery in ngtcp2.
    bool disable_pmtud = false;
};

/// Per-connection runtime limits plus transport setup shared by client and server.
struct connection_options {
    /// Maximum unsent application bytes buffered by the command runtime.
    size_t max_pending_send_bytes = 4 * 1024 * 1024;

    /// Maximum unread application bytes buffered across streams.
    size_t max_pending_receive_bytes = 4 * 1024 * 1024;

    /// QUIC transport parameters and ngtcp2 settings for the connection.
    transport_config transport{};
};

/// Options for locally opening a new QUIC stream.
struct stream_open_options {
    /// Directionality of the new stream.
    stream_type type = stream_type::bidirectional;
};

namespace internal {
class connection_state;
}

/// Movable handle to a single QUIC stream.
class stream final {
public:
    /// Constructs an empty stream handle.
    stream();
    ~stream();

    stream(stream&&) noexcept;
    stream& operator=(stream&&) noexcept;

    stream(const stream&) = delete;
    stream& operator=(const stream&) = delete;

    /// Returns true while at least one usable stream direction remains open.
    bool is_open() const noexcept;

    /// Returns the ngtcp2 stream id, or invalid_stream_id for an empty handle.
    stream_id id() const noexcept;

    /// Returns whether this is a bidirectional or unidirectional stream.
    stream_type type() const noexcept;

    /// Returns true if this endpoint may read bytes from the stream.
    ///
    /// This is an open/close capability accessor, not a readiness notification.
    bool can_read() const noexcept;

    /// Returns true if this endpoint may write bytes to the stream.
    ///
    /// This is an open/close capability accessor, not a readiness notification.
    bool can_write() const noexcept;

    /// Creates an input_stream for the readable side of this stream.
    input_stream<char> input(connected_socket_input_stream_config cfg = {});

    /// Creates an output_stream for the writable side of this stream.
    output_stream<char> output(size_t buffer_size = 8192);

    /// Sends FIN for the writable side of this stream.
    future<> close_output();

    /// Resets the stream with an application error code.
    future<> reset(application_error_code app_error_code = 0);

    /// Asks the peer to stop sending on the readable side of this stream.
    future<> stop_sending(application_error_code app_error_code = 0);

    /// Completes after the readable side reaches FIN, reset, or stop-sending shutdown.
    future<> wait_input_shutdown();

private:
    class impl;
    explicit stream(std::unique_ptr<impl> state);

    std::unique_ptr<impl> _impl;

    friend class connection;
    friend class internal::connection_state;
    friend connected_socket to_connected_socket(stream&& s);
};

/// Movable handle to an established QUIC connection.
class connection final {
public:
    /// Constructs an empty connection handle.
    connection();
    ~connection();

    connection(connection&&) noexcept;
    connection& operator=(connection&&) noexcept;

    connection(const connection&) = delete;
    connection& operator=(const connection&) = delete;

    /// Returns true while the underlying transport accepts new operations.
    bool is_open() const noexcept;

    /// Returns the local UDP address used by the connection.
    socket_address local_address() const;

    /// Returns the peer UDP address used by the connection.
    socket_address peer_address() const;

    /// Returns the ALPN selected by the TLS handshake.
    sstring selected_alpn() const;

    /// Opens a locally-initiated stream.
    future<stream> open_stream(stream_open_options options = {});

    /// Waits for the next peer-initiated stream.
    future<stream> accept_stream();

    /// Closes the connection and its streams.
    future<> close();

private:
    class impl;
    explicit connection(std::unique_ptr<impl> state);

    std::unique_ptr<impl> _impl;

    friend class quic_client;
    friend class quic_server;
};

/// Converts a bidirectional QUIC stream into a connected_socket wrapper.
connected_socket to_connected_socket(stream&& s);

} // namespace seastar::quic::experimental
