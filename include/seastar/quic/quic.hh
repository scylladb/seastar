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

using stream_id = int64_t;
using application_error_code = uint64_t;
inline constexpr stream_id invalid_stream_id = -1;

enum class stream_type : uint8_t {
    bidirectional,
    unidirectional,
};

enum class congestion_control_algorithm : uint8_t {
    reno,
    cubic,
    bbr,
    bbr2,
};

// Transport-level knobs passed to ngtcp2 when a connection is created.
struct transport_config {
    uint64_t max_idle_timeout_ns = 60ULL * 1000 * 1000 * 1000;
    uint64_t initial_max_stream_data_bidi_local = 256 * 1024;
    uint64_t initial_max_stream_data_bidi_remote = 256 * 1024;
    uint64_t initial_max_stream_data_uni = 256 * 1024;
    uint64_t initial_max_data = 4 * 1024 * 1024;
    uint64_t initial_max_streams_bidi = 128;
    uint64_t initial_max_streams_uni = 128;
    std::optional<size_t> max_tx_udp_payload_size{};
    std::optional<uint64_t> max_udp_payload_size{};
    std::optional<uint64_t> initial_rtt_ns{};
    std::optional<uint64_t> max_window{};
    std::optional<uint64_t> max_stream_window{};
    std::optional<size_t> ack_thresh{};
    std::optional<congestion_control_algorithm> congestion_control{};
    bool disable_tx_udp_payload_size_shaping = false;
    bool disable_pmtud = false;
};

// Per-connection runtime limits plus transport setup shared by client and server.
struct connection_options {
    size_t max_pending_send_bytes = 4 * 1024 * 1024;
    size_t max_pending_receive_bytes = 4 * 1024 * 1024;
    transport_config transport{};
};

// Options for locally opening a new QUIC stream.
struct stream_open_options {
    stream_type type = stream_type::bidirectional;
};

namespace internal {
class connection_state;
}

// Public handle to a single QUIC stream.
class stream final {
public:
    stream();
    ~stream();

    stream(stream&&) noexcept;
    stream& operator=(stream&&) noexcept;

    stream(const stream&) = delete;
    stream& operator=(const stream&) = delete;

    bool is_open() const noexcept;
    stream_id id() const noexcept;
    stream_type type() const noexcept;
    bool can_read() const noexcept;
    bool can_write() const noexcept;

    input_stream<char> input(connected_socket_input_stream_config cfg = {});
    output_stream<char> output(size_t buffer_size = 8192);

    future<> close_output();
    future<> reset(application_error_code app_error_code = 0);
    future<> stop_sending(application_error_code app_error_code = 0);
    future<> wait_input_shutdown();

private:
    class impl;
    explicit stream(std::unique_ptr<impl> state);

    std::unique_ptr<impl> _impl;

    friend class connection;
    friend class internal::connection_state;
    friend connected_socket to_connected_socket(stream&& s);
};

// Public handle to an established QUIC connection.
class connection final {
public:
    connection();
    ~connection();

    connection(connection&&) noexcept;
    connection& operator=(connection&&) noexcept;

    connection(const connection&) = delete;
    connection& operator=(const connection&) = delete;

    bool is_open() const noexcept;
    socket_address local_address() const;
    socket_address peer_address() const;
    sstring selected_alpn() const;

    future<stream> open_stream(stream_open_options options = {});
    future<stream> accept_stream();
    future<> close();

private:
    class impl;
    explicit connection(std::unique_ptr<impl> state);

    std::unique_ptr<impl> _impl;

    friend class quic_client;
    friend class quic_server;
};

connected_socket to_connected_socket(stream&& s);

} // namespace seastar::quic::experimental
