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

/// \brief Experimental QUIC transport API backed by ngtcp2 and GnuTLS.
///
/// The API follows Seastar's shard-per-core model. Handles are movable,
/// non-copyable, and must be used on the shard on which they were created.
namespace seastar::quic::experimental {

/// \brief Signed representation of a QUIC stream identifier as defined by RFC 9000.
using stream_id = int64_t;

/// \brief Application-defined QUIC stream or connection error code.
using application_error_code = uint64_t;

/// \brief Stream identifier returned by stream::id() for an empty stream handle.
inline constexpr stream_id invalid_stream_id = -1;

/// \brief Directionality of a QUIC stream.
enum class stream_type : uint8_t {
    bidirectional, ///< Both endpoints can send data.
    unidirectional, ///< Only the endpoint that initiated the stream can send data.
};

/// \brief Congestion controller selected for new QUIC connections.
enum class congestion_control_algorithm : uint8_t {
    reno, ///< NewReno congestion control.
    cubic, ///< CUBIC congestion control.
    bbr, ///< BBR congestion control.
    bbr2, ///< BBRv2 congestion control.
};

/// \brief Transport-level knobs passed to ngtcp2 when a connection is created.
///
/// The fields mirror members of `ngtcp2_transport_params` and
/// `ngtcp2_settings`. See the ngtcp2 API documentation for protocol-level
/// details. An unset optional keeps ngtcp2's default value.
struct transport_config {
    /// \brief Maximum permitted idle period.
    ///
    /// A zero duration disables the idle timeout. The effective timeout is
    /// negotiated with the peer according to RFC 9000.
    std::chrono::nanoseconds max_idle_timeout = std::chrono::seconds{60};

    /// \brief Initial receive window for each locally initiated bidirectional stream.
    ///
    /// This limits how many bytes the peer may send on such a stream.
    uint64_t initial_max_stream_data_bidi_local = 256 * 1024;

    /// \brief Initial receive window for each peer-initiated bidirectional stream.
    ///
    /// This limits how many bytes the peer may send on such a stream.
    uint64_t initial_max_stream_data_bidi_remote = 256 * 1024;

    /// \brief Initial receive window for each peer-initiated unidirectional stream.
    uint64_t initial_max_stream_data_uni = 256 * 1024;

    /// \brief Initial connection-wide receive flow-control window.
    uint64_t initial_max_data = 4 * 1024 * 1024;

    /// \brief Initial limit on concurrent bidirectional streams opened by the peer.
    uint64_t initial_max_streams_bidi = 128;

    /// \brief Initial limit on concurrent unidirectional streams opened by the peer.
    uint64_t initial_max_streams_uni = 128;

    /// \brief Maximum UDP payload size, in bytes, that this endpoint may transmit.
    ///
    /// Values, when set, must be in the inclusive range [1200, 65527].
    std::optional<size_t> max_tx_udp_payload_size{};

    /// \brief Maximum UDP payload size, in bytes, advertised as receivable.
    ///
    /// Values, when set, must be in the inclusive range [1200, 65527] and must
    /// not be smaller than max_tx_udp_payload_size.
    std::optional<uint64_t> max_udp_payload_size{};

    /// \brief Initial round-trip-time estimate used before measurements are available.
    std::optional<std::chrono::nanoseconds> initial_rtt{};

    /// \brief Maximum connection receive window when ngtcp2 auto-tuning is enabled.
    ///
    /// A zero value disables connection-level window auto-tuning.
    std::optional<uint64_t> max_window{};

    /// \brief Maximum per-stream receive window when ngtcp2 auto-tuning is enabled.
    ///
    /// A zero value disables stream-level window auto-tuning.
    std::optional<uint64_t> max_stream_window{};

    /// \brief Number of ACK-eliciting packets that triggers an immediate ACK.
    std::optional<size_t> ack_thresh{};

    /// \brief Congestion controller used for the connection.
    ///
    /// BBR and BBRv2 fall back to CUBIC when max_tx_udp_payload_size is at most
    /// 4096 bytes.
    std::optional<congestion_control_algorithm> congestion_control{};

    /// \brief Disable ngtcp2's transmit UDP payload-size shaping.
    bool disable_tx_udp_payload_size_shaping = false;

    /// \brief Disable path MTU discovery in ngtcp2.
    bool disable_pmtud = false;
};

/// \brief Per-connection runtime limits plus transport setup shared by client and server.
struct connection_options {
    /// \brief Maximum unsent application bytes buffered across all streams.
    ///
    /// A write waits for buffer space when accepting it would exceed this
    /// limit. Zero disables the byte limit.
    size_t max_pending_send_bytes = 4 * 1024 * 1024;

    /// \brief Maximum unread application bytes buffered across all streams.
    ///
    /// Exceeding this limit fails the affected stream input while leaving the
    /// other streams and the connection usable. Nonzero values also cap each
    /// initial receive window advertised to the peer. Zero disables the byte
    /// limit.
    size_t max_pending_receive_bytes = 4 * 1024 * 1024;

    /// \brief QUIC transport parameters and ngtcp2 settings for the connection.
    transport_config transport{};
};

/// \brief Options for locally opening a new QUIC stream.
struct stream_open_options {
    /// \brief Directionality of the new stream.
    stream_type type = stream_type::bidirectional;
};

/// \cond internal
namespace internal {
class connection_state;
}
/// \endcond

/// \brief Movable handle to a single QUIC stream.
///
/// A stream can be bidirectional or unidirectional. For a unidirectional
/// stream, the initiating endpoint can only write and the peer can only read.
/// Destroying the handle releases local ownership; use close_output(), reset(),
/// or stop_sending() when a protocol-visible shutdown is required.
class stream final {
public:
    /// \brief Construct an empty, closed stream handle.
    stream();

    /// \brief Destroy the stream handle.
    ~stream();

    /// \brief Move a stream handle.
    stream(stream&&) noexcept;

    /// \brief Replace this handle with another stream handle.
    stream& operator=(stream&&) noexcept;

    /// \brief Streams cannot be copied.
    stream(const stream&) = delete;

    /// \brief Streams cannot be copy-assigned.
    stream& operator=(const stream&) = delete;

    /// \brief Test whether at least one supported stream direction is still open.
    ///
    /// \return `false` for an empty handle, after transport shutdown, or after
    ///         all locally usable directions have shut down.
    bool is_open() const noexcept;

    /// \brief Get the QUIC stream identifier.
    ///
    /// \return The RFC 9000 stream identifier, or invalid_stream_id for an
    ///         empty handle.
    stream_id id() const noexcept;

    /// \brief Get the stream's protocol directionality.
    ///
    /// \return The stream type. An empty handle returns
    ///         stream_type::bidirectional.
    stream_type type() const noexcept;

    /// \brief Test whether the stream has a receive direction at this endpoint.
    ///
    /// This reports an invariant property of the stream direction. It is not an
    /// I/O-readiness notification and does not become `false` after FIN, reset,
    /// stop_sending(), or transport shutdown.
    ///
    /// \return `true` for bidirectional streams and peer-initiated
    ///         unidirectional streams; `false` otherwise and for empty handles.
    bool can_read() const noexcept;

    /// \brief Test whether the stream has a send direction at this endpoint.
    ///
    /// This reports an invariant property of the stream direction. It is not an
    /// I/O-readiness notification and does not become `false` after FIN, reset,
    /// or transport shutdown.
    ///
    /// \return `true` for bidirectional streams and locally initiated
    ///         unidirectional streams; `false` otherwise and for empty handles.
    bool can_write() const noexcept;

    /// \brief Create buffered input for the receive direction.
    ///
    /// Reads complete with EOF after a peer FIN. A peer reset, local
    /// stop_sending(), receive-buffer overflow, or transport failure makes reads
    /// fail with quic_error.
    ///
    /// \param cfg Input buffering configuration.
    /// \return An input stream backed by this QUIC stream.
    /// \throws quic_error If this handle is empty or has no receive direction.
    input_stream<char> input(connected_socket_input_stream_config cfg = {});

    /// \brief Create buffered output for the send direction.
    ///
    /// Closing the returned output stream flushes buffered data and sends FIN.
    ///
    /// \param buffer_size Output buffer size in bytes.
    /// \return An output stream backed by this QUIC stream.
    /// \throws quic_error If this handle is empty, has no send direction, or its
    ///                    send direction is already closed.
    output_stream<char> output(size_t buffer_size = 8192);

    /// \brief Send FIN and close the local send direction.
    ///
    /// The operation is idempotent. The returned future fails with quic_error
    /// if the stream has no send direction or the transport fails.
    future<> close_output();

    /// \brief Abort the local send direction with RESET_STREAM.
    ///
    /// \param app_error_code Application protocol error sent to the peer.
    /// \return A future that resolves after the reset is submitted to the
    ///         transport. Repeated calls after shutdown have no effect.
    future<> reset(application_error_code app_error_code = 0);

    /// \brief Abort local input and ask the peer to stop sending.
    ///
    /// Pending and subsequent reads fail after this call.
    ///
    /// \param app_error_code Application protocol error sent in STOP_SENDING.
    /// \return A future that resolves after STOP_SENDING is submitted to the
    ///         transport. Repeated calls after input shutdown have no effect.
    future<> stop_sending(application_error_code app_error_code = 0);

    /// \brief Wait for the receive direction to shut down.
    ///
    /// \return A future that resolves after peer FIN, peer reset, local
    ///         stop_sending(), receive failure, or transport shutdown.
    future<> wait_input_shutdown();

private:
    class impl;
    explicit stream(std::unique_ptr<impl> state);

    std::unique_ptr<impl> _impl;

    friend class connection;
    friend class internal::connection_state;
    friend connected_socket to_connected_socket(stream&& s);
};

/// \brief Movable handle to an established QUIC connection.
///
/// The connection owns its streams and transport state. It may be used only on
/// its originating shard.
class connection final {
public:
    /// \brief Construct an empty, closed connection handle.
    connection();

    /// \brief Destroy the connection handle.
    ~connection();

    /// \brief Move a connection handle.
    connection(connection&&) noexcept;

    /// \brief Replace this handle with another connection handle.
    connection& operator=(connection&&) noexcept;

    /// \brief Connections cannot be copied.
    connection(const connection&) = delete;

    /// \brief Connections cannot be copy-assigned.
    connection& operator=(const connection&) = delete;

    /// \brief Test whether the transport can accept new operations.
    ///
    /// \return `false` for an empty, closing, closed, or failed connection.
    bool is_open() const noexcept;

    /// \brief Get the local UDP endpoint of the active path.
    ///
    /// \return The local endpoint, or an empty address for an empty handle.
    socket_address local_address() const;

    /// \brief Get the peer UDP endpoint of the active path.
    ///
    /// \return The peer endpoint, or an empty address for an empty handle.
    socket_address peer_address() const;

    /// \brief Get the application protocol selected by the TLS handshake.
    ///
    /// \return The selected ALPN identifier, or an empty string for an empty
    ///         handle.
    sstring selected_alpn() const;

    /// \brief Open a locally initiated stream.
    ///
    /// The future waits when the peer's concurrent-stream limit is exhausted
    /// and resumes after stream credit becomes available.
    ///
    /// \param options Directionality and other stream-open options.
    /// \return A future containing the newly opened stream.
    future<stream> open_stream(stream_open_options options = {});

    /// \brief Wait for the next peer-initiated stream.
    ///
    /// \return A future containing streams in the order in which the transport
    ///         makes them visible. It fails with quic_error when the connection
    ///         closes or fails.
    future<stream> accept_stream();

    /// \brief Gracefully close the connection and all of its streams.
    ///
    /// This operation is idempotent. An empty connection is already closed.
    future<> close();

private:
    class impl;
    explicit connection(std::unique_ptr<impl> state);

    std::unique_ptr<impl> _impl;

    friend class quic_client;
    friend class quic_server;
};

/// \brief Adapt a bidirectional QUIC stream to Seastar's connected_socket interface.
///
/// The operation consumes \p s. The returned socket has the connection's UDP
/// endpoints as its local and remote addresses. Socket shutdown maps to QUIC
/// FIN, RESET_STREAM, and STOP_SENDING operations.
///
/// \param s Bidirectional stream to consume.
/// \return A connected socket owning the stream.
/// \throws quic_error If \p s is empty or lacks either direction.
connected_socket to_connected_socket(stream&& s);

} // namespace seastar::quic::experimental
