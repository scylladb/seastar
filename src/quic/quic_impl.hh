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

#include "quic_error_impl.hh"

#include <seastar/quic/quic.hh>

namespace seastar::quic::experimental {

namespace internal {
class stream_state;
class connection_state;

using stream_state_ptr = shared_ptr<stream_state>;
using connection_state_ptr = shared_ptr<connection_state>;
}

class stream::impl {
public:
    explicit impl(internal::stream_state_ptr state)
        : state(std::move(state)) {
    }

    internal::stream_state_ptr state;
};

class connection::impl {
public:
    explicit impl(internal::connection_state_ptr state)
        : state(std::move(state)) {
    }

    internal::connection_state_ptr state;
};

} // namespace seastar::quic::experimental

namespace seastar::quic::experimental::internal {

class command_runtime;
class stream_state;
class connection_state;

using command_runtime_ptr = shared_ptr<command_runtime>;
using stream_state_ptr = shared_ptr<stream_state>;
using connection_state_ptr = shared_ptr<connection_state>;

// Payload scheduled by the command runtime to be written by the transport.
struct quic_message {
    stream_id stream = invalid_stream_id;
    temporary_buffer<char> payload;
    bool fin = false;
};

enum class stream_shutdown_side : uint8_t {
    // Stop delivering bytes to the local reader.
    read,
    // Stop accepting bytes from the local writer.
    write,
};

// Commands emitted by the command runtime and executed by the transport-facing actor loop.
struct transport_command {
    enum class kind : uint8_t {
        // Write stream bytes, optionally carrying FIN.
        send,
        // Allocate a locally initiated QUIC stream id.
        open_stream,
        // Return receive credit after the application consumes data.
        consume_stream_data,
        // Abort the local write side with RESET_STREAM.
        reset_stream,
        // Ask the peer to stop sending on the local read side.
        stop_sending,
        // Start connection shutdown from the transport actor.
        close_connection,
    };

    kind op = kind::send;
    quic_message msg;
    stream_type type = stream_type::bidirectional;
    size_t consumed_bytes = 0;
    application_error_code app_error_code = 0;
    std::shared_ptr<promise<stream_id>> open_result;
};

// Bridge between the public stream API and the transport actor.
class command_runtime {
// The runtime owns user-facing promises and queues; the actor owns ngtcp2.
public:
    virtual ~command_runtime() = default;

    virtual bool is_open() const noexcept = 0;
    virtual socket_address local_address() const = 0;
    virtual socket_address peer_address() const = 0;
    virtual sstring selected_alpn() const = 0;

    virtual future<> send(quic_message msg) = 0;
    virtual future<stream_id> open_stream(stream_type type) = 0;
    virtual void complete_send_bytes(size_t len) = 0;
    virtual void consume_stream_data(stream_id sid, size_t len) = 0;
    virtual future<> reset_stream(stream_id sid, application_error_code app_error_code) = 0;
    virtual future<> stop_sending(stream_id sid, application_error_code app_error_code) = 0;
    virtual future<> close() = 0;

    virtual bool has_pending_commands() const noexcept = 0;
    virtual std::optional<transport_command> poll_command() = 0;
    virtual void set_command_notifier(std::function<void()> notifier) = 0;
    virtual void complete_open_stream(std::shared_ptr<promise<stream_id>> result, stream_id sid) = 0;
    virtual void fail_open_stream(std::shared_ptr<promise<stream_id>> result, quic_error_code error, sstring detail) = 0;
    virtual void mark_transport_ready(socket_address local, socket_address peer, sstring selected_alpn) = 0;
    virtual void mark_transport_closed() = 0;
    virtual void mark_error(quic_error_code error, sstring detail) = 0;
    virtual bool transport_terminal() const noexcept = 0;
    virtual bool transport_failed() const noexcept = 0;
    virtual quic_error_code transport_error() const noexcept = 0;
    virtual sstring transport_error_detail() const = 0;
};

struct transport_stream_write_result {
    int64_t nwrite = 0;
    size_t consumed = 0;
};

struct transport_open_stream_result {
    int rv = 0;
    stream_id sid = invalid_stream_id;
};

// Callback-based view of client/server transport operations shared by common helpers.
struct connection_transport {
    // Type-erased callback table lets client and server share transport helpers.
    void* ctx = nullptr;

    bool (*transport_active_fn)(void*) noexcept = nullptr;
    bool (*has_transport_connection_fn)(void*) noexcept = nullptr;
    bool (*can_retry_blocked_open_streams_fn)(void*) noexcept = nullptr;
    size_t (*tx_payload_limit_bytes_fn)(void*) noexcept = nullptr;

    int64_t (*write_pending_packet_fn)(void*, uint8_t*, size_t) = nullptr;
    transport_stream_write_result (*write_stream_packet_fn)(void*, stream_id, const char*, size_t, bool, uint8_t*, size_t) = nullptr;
    transport_open_stream_result (*try_open_stream_fn)(void*, stream_type) = nullptr;
    void (*retain_stream_data_fn)(void*, stream_id, temporary_buffer<char>) = nullptr;
    void (*complete_send_bytes_fn)(void*, size_t) = nullptr;
    int (*consume_stream_data_fn)(void*, stream_id, size_t) = nullptr;
    int (*shutdown_stream_write_fn)(void*, stream_id, application_error_code) = nullptr;
    int (*shutdown_stream_read_fn)(void*, stream_id, application_error_code) = nullptr;
    int (*read_transport_datagram_fn)(void*, const socket_address&, const char*, size_t) = nullptr;
    void (*sync_transport_path_fn)(void*) = nullptr;
    uint64_t (*transport_expiry_ns_fn)(void*) noexcept = nullptr;
    int (*handle_transport_expiry_fn)(void*, uint64_t) = nullptr;
    temporary_buffer<char>& (*tx_packet_buffer_fn)(void*) = nullptr;

    future<> (*send_datagram_packet_fn)(void*, temporary_buffer<char>) = nullptr;
    bool (*can_send_connection_close_fn)(void*) noexcept = nullptr;
    int64_t (*write_connection_close_packet_fn)(void*, uint8_t*, size_t) = nullptr;
    void (*on_stream_write_closed_fn)(void*, stream_id) = nullptr;
    void (*rearm_transport_timer_fn)(void*) = nullptr;
    void (*request_close_fn)(void*) = nullptr;
    void (*stop_transport_fn)(void*) = nullptr;
    void (*fail_transport_fn)(void*, quic_error_code, sstring) = nullptr;

    void (*complete_open_stream_fn)(void*, std::shared_ptr<promise<stream_id>>, stream_id) = nullptr;
    void (*fail_open_stream_fn)(void*, std::shared_ptr<promise<stream_id>>, quic_error_code, sstring) = nullptr;
    void (*defer_blocked_open_stream_fn)(void*, transport_command) = nullptr;
    std::optional<transport_command> (*pop_blocked_open_stream_fn)(void*, stream_type) = nullptr;
    bool (*blocked_open_stream_retry_pending_fn)(void*, stream_type) noexcept = nullptr;
    void (*clear_blocked_open_stream_retry_fn)(void*, stream_type) noexcept = nullptr;

    bool transport_active() const noexcept { return transport_active_fn(ctx); }
    bool has_transport_connection() const noexcept { return has_transport_connection_fn(ctx); }
    bool can_retry_blocked_open_streams() const noexcept { return can_retry_blocked_open_streams_fn(ctx); }
    size_t tx_payload_limit_bytes() const noexcept { return tx_payload_limit_bytes_fn(ctx); }

    int64_t write_pending_packet(uint8_t* outbuf, size_t outbuf_size) { return write_pending_packet_fn(ctx, outbuf, outbuf_size); }
    transport_stream_write_result write_stream_packet(
      stream_id sid,
      const char* data,
      size_t len,
      bool fin,
      uint8_t* outbuf,
      size_t outbuf_size) {
        return write_stream_packet_fn(ctx, sid, data, len, fin, outbuf, outbuf_size);
    }
    transport_open_stream_result try_open_stream(stream_type type) { return try_open_stream_fn(ctx, type); }
    void retain_stream_data(stream_id sid, temporary_buffer<char> payload) { retain_stream_data_fn(ctx, sid, std::move(payload)); }
    void complete_send_bytes(size_t len) { complete_send_bytes_fn(ctx, len); }
    int consume_stream_data(stream_id sid, size_t len) { return consume_stream_data_fn(ctx, sid, len); }
    int shutdown_stream_write(stream_id sid, application_error_code app_error_code) { return shutdown_stream_write_fn(ctx, sid, app_error_code); }
    int shutdown_stream_read(stream_id sid, application_error_code app_error_code) { return shutdown_stream_read_fn(ctx, sid, app_error_code); }
    int read_transport_datagram(const socket_address& src, const char* data, size_t len) { return read_transport_datagram_fn(ctx, src, data, len); }
    void sync_transport_path() { sync_transport_path_fn(ctx); }
    uint64_t transport_expiry_ns() const noexcept { return transport_expiry_ns_fn(ctx); }
    int handle_transport_expiry(uint64_t now_ns) { return handle_transport_expiry_fn(ctx, now_ns); }
    temporary_buffer<char>& tx_packet_buffer() { return tx_packet_buffer_fn(ctx); }

    future<> send_datagram_packet(temporary_buffer<char> packet) { return send_datagram_packet_fn(ctx, std::move(packet)); }
    bool can_send_connection_close() const noexcept { return can_send_connection_close_fn(ctx); }
    int64_t write_connection_close_packet(uint8_t* outbuf, size_t outbuf_size) { return write_connection_close_packet_fn(ctx, outbuf, outbuf_size); }
    void on_stream_write_closed(stream_id sid) { on_stream_write_closed_fn(ctx, sid); }
    void rearm_transport_timer() { rearm_transport_timer_fn(ctx); }
    void request_close() { request_close_fn(ctx); }
    void stop_transport() { stop_transport_fn(ctx); }
    void fail_transport(quic_error_code error, sstring detail) { fail_transport_fn(ctx, error, std::move(detail)); }

    void complete_open_stream(std::shared_ptr<promise<stream_id>> result, stream_id sid) { complete_open_stream_fn(ctx, std::move(result), sid); }
    void fail_open_stream(
      std::shared_ptr<promise<stream_id>> result,
      quic_error_code error,
      sstring detail) {
        fail_open_stream_fn(ctx, std::move(result), error, std::move(detail));
    }
    void defer_blocked_open_stream(transport_command cmd) { defer_blocked_open_stream_fn(ctx, std::move(cmd)); }
    std::optional<transport_command> pop_blocked_open_stream(stream_type type) { return pop_blocked_open_stream_fn(ctx, type); }
    bool blocked_open_stream_retry_pending(stream_type type) const noexcept { return blocked_open_stream_retry_pending_fn(ctx, type); }
    void clear_blocked_open_stream_retry(stream_type type) noexcept { clear_blocked_open_stream_retry_fn(ctx, type); }
};

template <typename Owner>
connection_transport make_connection_transport(Owner& owner) {
    // Owner methods stay concrete while common code depends only on connection_transport.
    return connection_transport{
      .ctx = &owner,
      .transport_active_fn = [] (void* ctx) noexcept { return static_cast<Owner*>(ctx)->transport_active(); },
      .has_transport_connection_fn = [] (void* ctx) noexcept { return static_cast<Owner*>(ctx)->has_transport_connection(); },
      .can_retry_blocked_open_streams_fn = [] (void* ctx) noexcept { return static_cast<Owner*>(ctx)->can_retry_blocked_open_streams(); },
      .tx_payload_limit_bytes_fn = [] (void* ctx) noexcept { return static_cast<Owner*>(ctx)->tx_payload_limit_bytes(); },
      .write_pending_packet_fn = [] (void* ctx, uint8_t* outbuf, size_t outbuf_size) { return static_cast<Owner*>(ctx)->write_pending_packet(outbuf, outbuf_size); },
      .write_stream_packet_fn = [] (void* ctx, stream_id sid, const char* data, size_t len, bool fin, uint8_t* outbuf, size_t outbuf_size) {
          return static_cast<Owner*>(ctx)->write_stream_packet(sid, data, len, fin, outbuf, outbuf_size);
      },
      .try_open_stream_fn = [] (void* ctx, stream_type type) { return static_cast<Owner*>(ctx)->try_open_stream(type); },
      .retain_stream_data_fn = [] (void* ctx, stream_id sid, temporary_buffer<char> payload) {
          static_cast<Owner*>(ctx)->retain_stream_data(sid, std::move(payload));
      },
      .complete_send_bytes_fn = [] (void* ctx, size_t len) { static_cast<Owner*>(ctx)->complete_send_bytes(len); },
      .consume_stream_data_fn = [] (void* ctx, stream_id sid, size_t len) { return static_cast<Owner*>(ctx)->consume_stream_data(sid, len); },
      .shutdown_stream_write_fn = [] (void* ctx, stream_id sid, application_error_code app_error_code) {
          return static_cast<Owner*>(ctx)->shutdown_stream_write(sid, app_error_code);
      },
      .shutdown_stream_read_fn = [] (void* ctx, stream_id sid, application_error_code app_error_code) {
          return static_cast<Owner*>(ctx)->shutdown_stream_read(sid, app_error_code);
      },
      .read_transport_datagram_fn = [] (void* ctx, const socket_address& src, const char* data, size_t len) {
          return static_cast<Owner*>(ctx)->read_transport_datagram(src, data, len);
      },
      .sync_transport_path_fn = [] (void* ctx) { static_cast<Owner*>(ctx)->sync_transport_path(); },
      .transport_expiry_ns_fn = [] (void* ctx) noexcept { return static_cast<Owner*>(ctx)->transport_expiry_ns(); },
      .handle_transport_expiry_fn = [] (void* ctx, uint64_t now_ns) { return static_cast<Owner*>(ctx)->handle_transport_expiry(now_ns); },
      .tx_packet_buffer_fn = [] (void* ctx) -> temporary_buffer<char>& { return static_cast<Owner*>(ctx)->tx_packet_buffer(); },
      .send_datagram_packet_fn = [] (void* ctx, temporary_buffer<char> packet) { return static_cast<Owner*>(ctx)->send_datagram_packet(std::move(packet)); },
      .can_send_connection_close_fn = [] (void* ctx) noexcept { return static_cast<Owner*>(ctx)->can_send_connection_close(); },
      .write_connection_close_packet_fn = [] (void* ctx, uint8_t* outbuf, size_t outbuf_size) {
          return static_cast<Owner*>(ctx)->write_connection_close_packet(outbuf, outbuf_size);
      },
      .on_stream_write_closed_fn = [] (void* ctx, stream_id sid) { static_cast<Owner*>(ctx)->on_stream_write_closed(sid); },
      .rearm_transport_timer_fn = [] (void* ctx) { static_cast<Owner*>(ctx)->rearm_transport_timer(); },
      .request_close_fn = [] (void* ctx) { static_cast<Owner*>(ctx)->request_close(); },
      .stop_transport_fn = [] (void* ctx) { static_cast<Owner*>(ctx)->stop_transport(); },
      .fail_transport_fn = [] (void* ctx, quic_error_code error, sstring detail) {
          static_cast<Owner*>(ctx)->fail_transport(error, std::move(detail));
      },
      .complete_open_stream_fn = [] (void* ctx, std::shared_ptr<promise<stream_id>> result, stream_id sid) {
          static_cast<Owner*>(ctx)->complete_open_stream(std::move(result), sid);
      },
      .fail_open_stream_fn = [] (void* ctx, std::shared_ptr<promise<stream_id>> result, quic_error_code error, sstring detail) {
          static_cast<Owner*>(ctx)->fail_open_stream(std::move(result), error, std::move(detail));
      },
      .defer_blocked_open_stream_fn = [] (void* ctx, transport_command cmd) {
          static_cast<Owner*>(ctx)->defer_blocked_open_stream(std::move(cmd));
      },
      .pop_blocked_open_stream_fn = [] (void* ctx, stream_type type) { return static_cast<Owner*>(ctx)->pop_blocked_open_stream(type); },
      .blocked_open_stream_retry_pending_fn = [] (void* ctx, stream_type type) noexcept {
          return static_cast<Owner*>(ctx)->blocked_open_stream_retry_pending(type);
      },
      .clear_blocked_open_stream_retry_fn = [] (void* ctx, stream_type type) noexcept {
          static_cast<Owner*>(ctx)->clear_blocked_open_stream_retry(type);
      },
    };
}

class connection_state {
public:
    explicit connection_state(command_runtime_ptr runtime, connection_options options = {});
    ~connection_state();

    connection_state(const connection_state&) = delete;
    connection_state& operator=(const connection_state&) = delete;

    bool is_open() const noexcept;
    socket_address local_address() const;
    socket_address peer_address() const;
    sstring selected_alpn() const;

    future<stream> open_stream(stream_open_options options = {});
    future<stream> accept_stream();
    future<> close();

    void on_stream_data(stream_id sid, stream_type type, bool peer_initiated, temporary_buffer<char> payload, bool fin);
    void on_stream_reset(stream_id sid, stream_type type, bool peer_initiated, application_error_code app_error_code);
    void on_stream_stop_sending(
      stream_id sid,
      stream_type type,
      bool peer_initiated,
      application_error_code app_error_code,
      stream_shutdown_side shutdown_side);
    void on_stream_closed(stream_id sid);
    void on_transport_closed(std::exception_ptr ex);

    future<> wait_for_actor_wakeup(bool has_pending_work, bool closing);
    void wake_actor();

    bool tick_pending() const noexcept;
    void clear_tick() noexcept;

    void arm_timer(std::chrono::nanoseconds delay, bool closing);
    void rearm_timer_from_expiry(uint64_t expiry_ns, uint64_t now_ns, bool closing);
    void cancel_timer() noexcept;

    void defer_blocked_open_stream(transport_command cmd);
    std::optional<transport_command> pop_blocked_open_stream(stream_type type);
    void request_blocked_open_stream_retry(stream_type type);
    bool blocked_open_stream_retry_pending(stream_type type) const noexcept;
    void clear_blocked_open_stream_retry(stream_type type) noexcept;
    bool has_blocked_open_stream_retry_work() const noexcept;
    void fail_blocked_open_streams(quic_error_code error, std::string_view detail);

private:
    class impl;
    std::unique_ptr<impl> _impl;
};

connection_state_ptr make_connection_state(command_runtime_ptr runtime, connection_options options = {});

future<> flush_pending_transport_packets(connection_transport& transport);
future<std::optional<quic_message>> send_stream_message(connection_transport& transport, quic_message msg);
future<bool> open_stream(connection_transport& transport, transport_command cmd);
future<> consume_stream_data(connection_transport& transport, stream_id sid, size_t len);
future<> reset_stream(connection_transport& transport, stream_id sid, application_error_code app_error_code);
future<> stop_sending(connection_transport& transport, stream_id sid, application_error_code app_error_code);
future<> retry_blocked_open_streams(connection_transport& transport, stream_type type);
future<std::optional<transport_command>> handle_transport_command(connection_transport& transport, transport_command cmd);
future<> recv_transport_datagram(connection_transport& transport, const socket_address& src, temporary_buffer<char> pkt);
future<> handle_transport_timer(connection_transport& transport);
future<> send_connection_close(connection_transport& transport);

command_runtime_ptr make_command_runtime(connection_options options = {});

} // namespace seastar::quic::experimental::internal
