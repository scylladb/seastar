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

#include <array>
#include <deque>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gnutls/gnutls.h>
#include <ngtcp2/ngtcp2.h>

#include <seastar/core/future.hh>
#include <seastar/core/iostream.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/core/temporary_buffer.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/when_all.hh>
#include <seastar/net/api.hh>
#include <seastar/quic/quic.hh>
#include <seastar/quic/quic_client.hh>
#include <seastar/quic/quic_server.hh>
#include <seastar/testing/test_case.hh>

#include "quic/quic_common.hh"
#include "quic/quic_error_impl.hh"
#include "quic/quic_impl.hh"

using namespace seastar;
using namespace seastar::quic::experimental;
namespace quic_internal = seastar::quic::experimental::internal;
using quic_stream = seastar::quic::experimental::stream;

namespace {

constexpr int ngtcp2_err_write_more = NGTCP2_ERR_WRITE_MORE;
constexpr int ngtcp2_err_stream_data_blocked = NGTCP2_ERR_STREAM_DATA_BLOCKED;
constexpr int ngtcp2_err_stream_id_blocked = NGTCP2_ERR_STREAM_ID_BLOCKED;
constexpr int ngtcp2_err_draining = NGTCP2_ERR_DRAINING;
constexpr int ngtcp2_err_stream_shut_wr = NGTCP2_ERR_STREAM_SHUT_WR;

class fake_connection_transport final : public quic_internal::connection_transport {
public:
    fake_connection_transport() {
        static_cast<quic_internal::connection_transport&>(*this) = quic_internal::make_connection_transport(*this);
    }

    bool active = true;
    bool has_connection = true;
    bool retry_blocked_open_streams = false;
    bool bidi_retry_pending = false;
    bool uni_retry_pending = false;
    bool can_close = false;
    size_t consume_calls = 0;
    size_t completed_send_bytes = 0;
    size_t stream_write_calls = 0;
    size_t write_pending_calls = 0;
    size_t send_datagram_calls = 0;
    size_t rearm_calls = 0;
    size_t open_stream_calls = 0;
    size_t complete_open_stream_calls = 0;
    size_t fail_open_stream_calls = 0;
    size_t deferred_open_stream_calls = 0;
    size_t reset_stream_calls = 0;
    size_t stop_sending_calls = 0;
    size_t stream_write_closed_calls = 0;
    size_t request_close_calls = 0;
    size_t stop_transport_calls = 0;
    size_t read_datagram_calls = 0;
    size_t sync_path_calls = 0;
    size_t timer_expiry_calls = 0;
    size_t write_connection_close_calls = 0;
    size_t clear_bidi_retry_calls = 0;
    size_t clear_uni_retry_calls = 0;
    stream_id consumed_sid = invalid_stream_id;
    size_t consumed_len = 0;
    stream_id reset_sid = invalid_stream_id;
    stream_id stop_sending_sid = invalid_stream_id;
    stream_id write_closed_sid = invalid_stream_id;
    application_error_code reset_error = 0;
    application_error_code stop_sending_error = 0;
    stream_id completed_open_sid = invalid_stream_id;
    int consume_result = 0;
    int reset_result = 0;
    int stop_sending_result = 0;
    int read_datagram_result = 0;
    int timer_expiry_result = 0;
    uint64_t expiry_ns = 0;
    int64_t connection_close_result = 0;
    temporary_buffer<char> tx_buffer = temporary_buffer<char>(128);
    std::deque<quic_internal::transport_stream_write_result> stream_write_results;
    std::deque<quic_internal::transport_open_stream_result> open_stream_results;
    std::deque<int64_t> pending_packet_results;
    std::deque<quic_internal::transport_command> deferred_bidi_open_streams;
    std::deque<quic_internal::transport_command> deferred_uni_open_streams;

    bool transport_active() const noexcept {
        return active;
    }

    bool has_transport_connection() const noexcept {
        return has_connection;
    }

    bool can_retry_blocked_open_streams() const noexcept {
        return retry_blocked_open_streams;
    }

    size_t tx_payload_limit_bytes() const noexcept {
        return 1200;
    }

    int64_t write_pending_packet(uint8_t*, size_t) {
        ++write_pending_calls;
        if (!pending_packet_results.empty()) {
            auto result = pending_packet_results.front();
            pending_packet_results.pop_front();
            return result;
        }
        return 0;
    }

    quic_internal::transport_stream_write_result write_stream_packet(
      stream_id,
      const char*,
      size_t,
      bool,
      uint8_t*,
      size_t) {
        ++stream_write_calls;
        if (!stream_write_results.empty()) {
            auto result = stream_write_results.front();
            stream_write_results.pop_front();
            return result;
        }
        return {};
    }

    quic_internal::transport_open_stream_result try_open_stream(stream_type) {
        ++open_stream_calls;
        if (!open_stream_results.empty()) {
            auto result = open_stream_results.front();
            open_stream_results.pop_front();
            return result;
        }
        return {};
    }

    void complete_send_bytes(size_t len) {
        completed_send_bytes += len;
    }

    void retain_stream_data(stream_id, temporary_buffer<char>) {
    }

    int consume_stream_data(stream_id sid, size_t len) {
        ++consume_calls;
        consumed_sid = sid;
        consumed_len = len;
        return consume_result;
    }

    int shutdown_stream_write(stream_id sid, application_error_code app_error_code) {
        ++reset_stream_calls;
        reset_sid = sid;
        reset_error = app_error_code;
        return reset_result;
    }

    int shutdown_stream_read(stream_id sid, application_error_code app_error_code) {
        ++stop_sending_calls;
        stop_sending_sid = sid;
        stop_sending_error = app_error_code;
        return stop_sending_result;
    }

    int read_transport_datagram(const socket_address&, const char*, size_t) {
        ++read_datagram_calls;
        return read_datagram_result;
    }

    void sync_transport_path() {
        ++sync_path_calls;
    }

    uint64_t transport_expiry_ns() const noexcept {
        return expiry_ns;
    }

    int handle_transport_expiry(uint64_t) {
        ++timer_expiry_calls;
        return timer_expiry_result;
    }

    temporary_buffer<char>& tx_packet_buffer() {
        return tx_buffer;
    }

    future<> send_datagram_packet(temporary_buffer<char>) {
        ++send_datagram_calls;
        return make_ready_future<>();
    }

    bool can_send_connection_close() const noexcept {
        return can_close;
    }

    int64_t write_connection_close_packet(uint8_t*, size_t) {
        ++write_connection_close_calls;
        return connection_close_result;
    }

    void on_stream_write_closed(stream_id sid) {
        ++stream_write_closed_calls;
        write_closed_sid = sid;
    }

    void rearm_transport_timer() {
        ++rearm_calls;
    }

    void request_close() {
        ++request_close_calls;
    }

    void stop_transport() {
        ++stop_transport_calls;
        active = false;
    }

    void fail_transport(quic_error_code error, sstring detail) {
        last_error = error;
        last_error_detail = std::move(detail);
    }

    void complete_open_stream(std::shared_ptr<promise<stream_id>> result, stream_id sid) {
        ++complete_open_stream_calls;
        completed_open_sid = sid;
        if (result) {
            result->set_value(sid);
        }
    }

    void fail_open_stream(std::shared_ptr<promise<stream_id>> result, quic_error_code error, sstring detail) {
        ++fail_open_stream_calls;
        open_stream_error = error;
        open_stream_error_detail = detail;
        if (result) {
            result->set_exception(std::make_exception_ptr(quic_error(error, std::string(detail))));
        }
    }

    void defer_blocked_open_stream(quic_internal::transport_command cmd) {
        ++deferred_open_stream_calls;
        auto& q = cmd.type == stream_type::bidirectional
                    ? deferred_bidi_open_streams
                    : deferred_uni_open_streams;
        q.push_back(std::move(cmd));
    }

    std::optional<quic_internal::transport_command> pop_blocked_open_stream(stream_type type) {
        auto& q = type == stream_type::bidirectional
                    ? deferred_bidi_open_streams
                    : deferred_uni_open_streams;
        if (q.empty()) {
            return std::nullopt;
        }
        auto cmd = std::move(q.front());
        q.pop_front();
        return cmd;
    }

    bool blocked_open_stream_retry_pending(stream_type type) const noexcept {
        return type == stream_type::bidirectional ? bidi_retry_pending : uni_retry_pending;
    }

    void clear_blocked_open_stream_retry(stream_type type) noexcept {
        if (type == stream_type::bidirectional) {
            ++clear_bidi_retry_calls;
            bidi_retry_pending = false;
        } else {
            ++clear_uni_retry_calls;
            uni_retry_pending = false;
        }
    }

    std::optional<quic_error_code> last_error;
    sstring last_error_detail;
    std::optional<quic_error_code> open_stream_error;
    sstring open_stream_error_detail;
};

quic_internal::quic_message make_message(stream_id sid, std::string_view payload, bool fin = false) {
    return quic_internal::quic_message{
      .stream = sid,
      .payload = temporary_buffer<char>::copy_of(payload),
      .fin = fin,
    };
}

template <typename Func>
void require_quic_error(Func&& func, quic_error_code error) {
    try {
        func();
        BOOST_FAIL("expected quic_error");
    } catch (const quic_error& e) {
        BOOST_REQUIRE(e.code() == error);
    }
}

template <typename Future>
void require_quic_future_exception(Future&& fut, quic_error_code error) {
    try {
        std::forward<Future>(fut).get();
        BOOST_FAIL("expected quic_error");
    } catch (const quic_error& e) {
        BOOST_REQUIRE(e.code() == error);
    }
}

quic_stream open_stream_from_engine(
  const quic_internal::command_runtime_ptr& runtime,
  const quic_internal::connection_state_ptr& engine,
  stream_id sid,
  stream_type type) {
    auto opened = engine->open_stream(stream_open_options{.type = type});
    auto cmd = runtime->poll_command();
    BOOST_REQUIRE(cmd.has_value());
    BOOST_REQUIRE(cmd->op == quic_internal::transport_command::kind::open_stream);
    BOOST_REQUIRE(cmd->open_result);
    BOOST_REQUIRE(cmd->type == type);
    runtime->complete_open_stream(cmd->open_result, sid);
    return opened.get();
}

socket_address allocate_loopback_quic_address() {
    auto probe = make_bound_datagram_channel(make_ipv4_address({0x7f000001, 0}));
    auto address = probe.local_address();
    probe.close();
    return address;
}

future<> echo_quic_stream(quic_stream stream) {
    auto input = stream.input();
    auto output = stream.output();
    while (true) {
        auto chunk = co_await input.read();
        if (chunk.empty()) {
            break;
        }
        co_await output.write(std::move(chunk));
        co_await output.flush();
    }
    co_await output.close();
    co_await input.close();
}

future<> run_quic_echo_session(quic_server& server, size_t streams_to_accept) {
    auto session = co_await server.accept();
    std::vector<future<>> stream_tasks;
    stream_tasks.reserve(streams_to_accept);
    for (size_t i = 0; i < streams_to_accept; ++i) {
        stream_tasks.push_back(echo_quic_stream(co_await session.accept_stream()));
    }
    co_await when_all_succeed(stream_tasks.begin(), stream_tasks.end());
}

future<> quic_echo_round_trip(connection& session, sstring payload) {
    auto quic_stream = co_await session.open_stream();
    auto input = quic_stream.input();
    auto output = quic_stream.output();

    co_await output.write(payload);
    co_await output.close();

    auto echoed = co_await input.read_exactly(payload.size());
    BOOST_REQUIRE_EQUAL(to_sstring(std::move(echoed)), payload);

    auto eof = co_await input.read();
    BOOST_REQUIRE(eof.empty());
    co_await input.close();
}

std::array<char, 4> make_frame_header(size_t size) {
    BOOST_REQUIRE_LE(size, std::numeric_limits<uint32_t>::max());
    uint32_t value = static_cast<uint32_t>(size);
    return {
      static_cast<char>((value >> 24) & 0xff),
      static_cast<char>((value >> 16) & 0xff),
      static_cast<char>((value >> 8) & 0xff),
      static_cast<char>(value & 0xff),
    };
}

uint32_t parse_frame_header(const temporary_buffer<char>& header) {
    BOOST_REQUIRE_EQUAL(header.size(), 4);
    const auto* p = reinterpret_cast<const unsigned char*>(header.get());
    return (static_cast<uint32_t>(p[0]) << 24)
           | (static_cast<uint32_t>(p[1]) << 16)
           | (static_cast<uint32_t>(p[2]) << 8)
           | static_cast<uint32_t>(p[3]);
}

future<sstring> read_quic_frame(input_stream<char>& input) {
    auto header = co_await input.read_exactly(4);
    auto size = parse_frame_header(header);
    auto payload = co_await input.read_exactly(size);
    BOOST_REQUIRE_EQUAL(payload.size(), size);
    co_return to_sstring(std::move(payload));
}

future<> write_quic_frame(output_stream<char>& output, const sstring& payload) {
    auto header = make_frame_header(payload.size());
    co_await output.write(header.data(), header.size());
    co_await output.write(payload);
    co_await output.flush();
}

future<> echo_quic_frame(quic_stream stream) {
    auto input = stream.input();
    auto output = stream.output();
    auto payload = co_await read_quic_frame(input);
    co_await write_quic_frame(output, payload);
}

future<> run_quic_framed_echo_session(connection& session, size_t streams_to_accept) {
    std::vector<future<>> stream_tasks;
    stream_tasks.reserve(streams_to_accept);
    for (size_t i = 0; i < streams_to_accept; ++i) {
        stream_tasks.push_back(echo_quic_frame(co_await session.accept_stream()));
    }
    co_await when_all_succeed(stream_tasks.begin(), stream_tasks.end());
}

future<> quic_framed_round_trip(connection& session, sstring payload) {
    auto stream = co_await session.open_stream();
    auto input = stream.input();
    auto output = stream.output();

    co_await write_quic_frame(output, payload);

    auto response = co_await read_quic_frame(input);
    BOOST_REQUIRE_EQUAL(response, payload);
}

sstring make_large_quic_payload(size_t size) {
    sstring payload;
    payload.resize(size);
    for (size_t i = 0; i < size; ++i) {
        payload[i] = static_cast<char>('a' + (i % 26));
    }
    return payload;
}

connection_options large_payload_options() {
    connection_options options;
    options.max_pending_send_bytes = 32 * 1024 * 1024;
    options.max_pending_receive_bytes = 32 * 1024 * 1024;
    options.transport.initial_max_stream_data_bidi_local = 16 * 1024 * 1024;
    options.transport.initial_max_stream_data_bidi_remote = 16 * 1024 * 1024;
    options.transport.initial_max_stream_data_uni = 16 * 1024 * 1024;
    options.transport.initial_max_data = 64 * 1024 * 1024;
    return options;
}

} // namespace

SEASTAR_TEST_CASE(test_quic_default_public_objects_are_safe) {
    return seastar::async([] {
        quic_stream s;
        BOOST_REQUIRE(!s.is_open());
        BOOST_REQUIRE_EQUAL(s.id(), invalid_stream_id);
        BOOST_REQUIRE(s.type() == stream_type::bidirectional);
        BOOST_REQUIRE(!s.can_read());
        BOOST_REQUIRE(!s.can_write());
        require_quic_error([&s] { (void)s.input(); }, quic_error::invalid_state);
        require_quic_error([&s] { (void)s.output(); }, quic_error::invalid_state);
        require_quic_future_exception(s.close_output(), quic_error::invalid_state);
        require_quic_future_exception(s.reset(), quic_error::invalid_state);
        require_quic_future_exception(s.stop_sending(), quic_error::invalid_state);
        require_quic_future_exception(s.wait_input_shutdown(), quic_error::invalid_state);
        require_quic_error([s = std::move(s)]() mutable { (void)to_connected_socket(std::move(s)); }, quic_error::invalid_state);

        connection c;
        BOOST_REQUIRE(!c.is_open());
        require_quic_future_exception(c.open_stream(), quic_error::invalid_state);
        require_quic_future_exception(c.accept_stream(), quic_error::invalid_state);
        c.close().get();
    });
}

SEASTAR_TEST_CASE(test_quic_client_server_echoes_concurrent_streams_on_one_connection) {
    auto address = allocate_loopback_quic_address();
    std::vector<sstring> messages = {
      "first request",
      "second request with a longer payload",
      "third request",
      "fourth request on the same connection",
      "fifth request",
    };

    quic_server server;
    quic_client client;
    std::optional<connection> session;
    std::optional<future<>> server_task;
    std::exception_ptr error;

    try {
        quic_server_config server_cfg;
        server_cfg.listen_address = address;
        server_cfg.crt_file = "test.crt";
        server_cfg.key_file = "test.key";
        co_await server.start(std::move(server_cfg));
        server_task.emplace(run_quic_echo_session(server, messages.size()));

        quic_client_config client_cfg;
        client_cfg.remote_address = address;
        client_cfg.server_name = "test.scylladb.org";
        client_cfg.ca_file = "test.crt";
        session.emplace(co_await client.connect(std::move(client_cfg)));

        std::vector<future<>> client_tasks;
        client_tasks.reserve(messages.size());
        for (auto& message : messages) {
            client_tasks.push_back(quic_echo_round_trip(*session, message));
        }
        co_await when_all_succeed(client_tasks.begin(), client_tasks.end());
    } catch (...) {
        error = std::current_exception();
    }

    if (session) {
        try {
            co_await session->close();
        } catch (...) {
            if (!error) {
                error = std::current_exception();
            }
        }
    }

    try {
        co_await client.stop();
    } catch (...) {
        if (!error) {
            error = std::current_exception();
        }
    }

    try {
        co_await server.stop();
    } catch (...) {
        if (!error) {
            error = std::current_exception();
        }
    }

    if (server_task) {
        try {
            co_await std::move(*server_task);
        } catch (...) {
            if (!error) {
                error = std::current_exception();
            }
        }
    }

    if (error) {
        std::rethrow_exception(error);
    }
}

SEASTAR_TEST_CASE(test_quic_client_server_handles_concurrent_framed_requests_on_one_connection) {
    auto address = allocate_loopback_quic_address();
    std::vector<sstring> messages = {
      "aaa",
      make_large_quic_payload(128 * 1024),
      make_large_quic_payload(1024 * 1024),
      "bbbbb",
      make_large_quic_payload(2 * 1024 * 1024),
      "cc",
      make_large_quic_payload(4 * 1024 * 1024),
      "dddddd",
    };

    quic_server server;
    quic_client client;
    std::optional<connection> session;
    std::optional<connection> server_session;
    std::optional<future<>> server_task;
    std::exception_ptr error;

    try {
        quic_server_config server_cfg;
        server_cfg.listen_address = address;
        server_cfg.crt_file = "test.crt";
        server_cfg.key_file = "test.key";
        server_cfg.session_options = large_payload_options();
        co_await server.start(std::move(server_cfg));
        auto server_accept = server.accept();

        quic_client_config client_cfg;
        client_cfg.remote_address = address;
        client_cfg.server_name = "test.scylladb.org";
        client_cfg.ca_file = "test.crt";
        client_cfg.session_options = large_payload_options();
        session.emplace(co_await client.connect(std::move(client_cfg)));
        server_session.emplace(co_await std::move(server_accept));
        server_task.emplace(run_quic_framed_echo_session(*server_session, messages.size()));

        std::vector<future<>> client_tasks;
        client_tasks.reserve(messages.size());
        for (auto& message : messages) {
            client_tasks.push_back(quic_framed_round_trip(*session, message));
        }
        co_await when_all_succeed(client_tasks.begin(), client_tasks.end());
        co_await std::move(*server_task);
        server_task.reset();
    } catch (...) {
        error = std::current_exception();
    }

    if (session) {
        try {
            co_await session->close();
        } catch (...) {
            if (!error) {
                error = std::current_exception();
            }
        }
    }

    if (server_session) {
        try {
            co_await server_session->close();
        } catch (...) {
            if (!error) {
                error = std::current_exception();
            }
        }
    }

    try {
        co_await client.stop();
    } catch (...) {
        if (!error) {
            error = std::current_exception();
        }
    }

    try {
        co_await server.stop();
    } catch (...) {
        if (!error) {
            error = std::current_exception();
        }
    }

    if (server_task) {
        try {
            co_await std::move(*server_task);
        } catch (...) {
            if (!error) {
                error = std::current_exception();
            }
        }
    }

    if (error) {
        std::rethrow_exception(error);
    }
}

SEASTAR_TEST_CASE(test_quic_client_server_handles_large_framed_request) {
    auto address = allocate_loopback_quic_address();
    auto message = make_large_quic_payload(8 * 1024 * 1024);

    quic_server server;
    quic_client client;
    std::optional<connection> session;
    std::optional<connection> server_session;
    std::optional<future<>> server_task;
    std::exception_ptr error;

    try {
        quic_server_config server_cfg;
        server_cfg.listen_address = address;
        server_cfg.crt_file = "test.crt";
        server_cfg.key_file = "test.key";
        server_cfg.session_options = large_payload_options();
        co_await server.start(std::move(server_cfg));
        auto server_accept = server.accept();

        quic_client_config client_cfg;
        client_cfg.remote_address = address;
        client_cfg.server_name = "test.scylladb.org";
        client_cfg.ca_file = "test.crt";
        client_cfg.session_options = large_payload_options();
        session.emplace(co_await client.connect(std::move(client_cfg)));
        server_session.emplace(co_await std::move(server_accept));
        server_task.emplace(run_quic_framed_echo_session(*server_session, 1));

        co_await quic_framed_round_trip(*session, message);
        co_await std::move(*server_task);
        server_task.reset();
    } catch (...) {
        error = std::current_exception();
    }

    if (session) {
        try {
            co_await session->close();
        } catch (...) {
            if (!error) {
                error = std::current_exception();
            }
        }
    }

    if (server_session) {
        try {
            co_await server_session->close();
        } catch (...) {
            if (!error) {
                error = std::current_exception();
            }
        }
    }

    try {
        co_await client.stop();
    } catch (...) {
        if (!error) {
            error = std::current_exception();
        }
    }

    try {
        co_await server.stop();
    } catch (...) {
        if (!error) {
            error = std::current_exception();
        }
    }

    if (server_task) {
        try {
            co_await std::move(*server_task);
        } catch (...) {
            if (!error) {
                error = std::current_exception();
            }
        }
    }

    if (error) {
        std::rethrow_exception(error);
    }
}

SEASTAR_TEST_CASE(test_quic_connection_metadata_tracks_transport_ready) {
    return seastar::async([] {
        auto runtime = quic_internal::make_command_runtime();
        auto engine = quic_internal::make_connection_state(runtime);
        auto local = socket_address(ipv4_addr("127.0.0.1", 1111));
        auto peer = socket_address(ipv4_addr("127.0.0.1", 2222));

        runtime->mark_transport_ready(local, peer, "h3");

        BOOST_REQUIRE(engine->is_open());
        BOOST_REQUIRE_EQUAL(engine->local_address(), local);
        BOOST_REQUIRE_EQUAL(engine->peer_address(), peer);
        BOOST_REQUIRE_EQUAL(engine->selected_alpn(), "h3");
    });
}

SEASTAR_TEST_CASE(test_quic_open_stream_exposes_directional_capabilities) {
    return seastar::async([] {
        auto runtime = quic_internal::make_command_runtime();
        auto engine = quic_internal::make_connection_state(runtime);

        auto bidi = open_stream_from_engine(runtime, engine, 0, stream_type::bidirectional);
        BOOST_REQUIRE(bidi.is_open());
        BOOST_REQUIRE_EQUAL(bidi.id(), 0);
        BOOST_REQUIRE(bidi.type() == stream_type::bidirectional);
        BOOST_REQUIRE(bidi.can_read());
        BOOST_REQUIRE(bidi.can_write());
        (void)bidi.input();
        (void)bidi.output();

        auto uni = open_stream_from_engine(runtime, engine, 2, stream_type::unidirectional);
        BOOST_REQUIRE(uni.is_open());
        BOOST_REQUIRE_EQUAL(uni.id(), 2);
        BOOST_REQUIRE(uni.type() == stream_type::unidirectional);
        BOOST_REQUIRE(!uni.can_read());
        BOOST_REQUIRE(uni.can_write());
        require_quic_error([&uni] { (void)uni.input(); }, quic_error::invalid_state);
        (void)uni.output();
        require_quic_error([uni = std::move(uni)]() mutable { (void)to_connected_socket(std::move(uni)); }, quic_error::invalid_state);
    });
}

SEASTAR_TEST_CASE(test_quic_accept_stream_exposes_peer_directional_capabilities_once) {
    return seastar::async([] {
        auto runtime = quic_internal::make_command_runtime();
        auto engine = quic_internal::make_connection_state(runtime);

        engine->on_stream_data(5, stream_type::unidirectional, true, temporary_buffer<char>("abc", 3), false);
        engine->on_stream_data(5, stream_type::unidirectional, true, temporary_buffer<char>("def", 3), false);

        auto accepted = engine->accept_stream().get();
        BOOST_REQUIRE(accepted.is_open());
        BOOST_REQUIRE_EQUAL(accepted.id(), 5);
        BOOST_REQUIRE(accepted.type() == stream_type::unidirectional);
        BOOST_REQUIRE(accepted.can_read());
        BOOST_REQUIRE(!accepted.can_write());
        (void)accepted.input();
        require_quic_error([&accepted] { (void)accepted.output(); }, quic_error::invalid_state);

        auto second_accept = engine->accept_stream();
        BOOST_REQUIRE(!second_accept.available());
        engine->on_transport_closed(std::make_exception_ptr(quic_error(quic_error::closed, "done")));
        require_quic_future_exception(std::move(second_accept), quic_error::closed);
    });
}

SEASTAR_TEST_CASE(test_quic_pending_accept_completes_when_peer_stream_arrives) {
    return seastar::async([] {
        auto runtime = quic_internal::make_command_runtime();
        auto engine = quic_internal::make_connection_state(runtime);

        auto pending_accept = engine->accept_stream();
        BOOST_REQUIRE(!pending_accept.available());

        engine->on_stream_data(7, stream_type::bidirectional, true, temporary_buffer<char>("hello", 5), false);

        auto accepted = pending_accept.get();
        BOOST_REQUIRE(accepted.is_open());
        BOOST_REQUIRE_EQUAL(accepted.id(), 7);
        BOOST_REQUIRE(accepted.type() == stream_type::bidirectional);
        BOOST_REQUIRE(accepted.can_read());
        BOOST_REQUIRE(accepted.can_write());

        auto input = accepted.input();
        auto chunk = input.read_exactly(5).get();
        BOOST_REQUIRE_EQUAL(to_sstring(std::move(chunk)), "hello");
    });
}

SEASTAR_TEST_CASE(test_quic_peer_stream_data_is_buffered_before_accept) {
    return seastar::async([] {
        auto runtime = quic_internal::make_command_runtime();
        auto engine = quic_internal::make_connection_state(runtime);

        engine->on_stream_data(9, stream_type::bidirectional, true, temporary_buffer<char>("abc", 3), false);
        engine->on_stream_data(9, stream_type::bidirectional, true, temporary_buffer<char>("def", 3), false);

        auto accepted = engine->accept_stream().get();
        BOOST_REQUIRE_EQUAL(accepted.id(), 9);

        auto input = accepted.input();
        auto payload = input.read_exactly(6).get();
        BOOST_REQUIRE_EQUAL(to_sstring(std::move(payload)), "abcdef");
    });
}

SEASTAR_TEST_CASE(test_quic_fin_completes_input_shutdown_and_eof) {
    return seastar::async([] {
        auto runtime = quic_internal::make_command_runtime();
        auto engine = quic_internal::make_connection_state(runtime);

        engine->on_stream_data(7, stream_type::bidirectional, true, temporary_buffer<char>("ping", 4), true);
        auto accepted = engine->accept_stream().get();
        auto shutdown = accepted.wait_input_shutdown();
        BOOST_REQUIRE(shutdown.available());
        shutdown.get();

        auto input = accepted.input();
        auto chunk = input.read().get();
        BOOST_REQUIRE_EQUAL(to_sstring(std::move(chunk)), "ping");
        auto eof = input.read().get();
        BOOST_REQUIRE(eof.empty());
    });
}

SEASTAR_TEST_CASE(test_quic_stream_close_reset_and_stop_sending_queue_commands) {
    return seastar::async([] {
        auto runtime = quic_internal::make_command_runtime();
        auto engine = quic_internal::make_connection_state(runtime);

        auto output_stream = open_stream_from_engine(runtime, engine, 9, stream_type::bidirectional);
        output_stream.close_output().get();
        auto close_cmd = runtime->poll_command();
        BOOST_REQUIRE(close_cmd.has_value());
        BOOST_REQUIRE(close_cmd->op == quic_internal::transport_command::kind::send);
        BOOST_REQUIRE_EQUAL(close_cmd->msg.stream, 9);
        BOOST_REQUIRE(close_cmd->msg.payload.empty());
        BOOST_REQUIRE(close_cmd->msg.fin);
        require_quic_error([&output_stream] { (void)output_stream.output(); }, quic_error::closed);

        auto reset_stream = open_stream_from_engine(runtime, engine, 11, stream_type::bidirectional);
        reset_stream.reset(123).get();
        auto reset_cmd = runtime->poll_command();
        BOOST_REQUIRE(reset_cmd.has_value());
        BOOST_REQUIRE(reset_cmd->op == quic_internal::transport_command::kind::reset_stream);
        BOOST_REQUIRE_EQUAL(reset_cmd->msg.stream, 11);
        BOOST_REQUIRE_EQUAL(reset_cmd->app_error_code, 123);

        engine->on_stream_data(13, stream_type::bidirectional, true, temporary_buffer<char>("payload", 7), false);
        auto input_stream = engine->accept_stream().get();
        input_stream.stop_sending(77).get();
        auto stop_cmd = runtime->poll_command();
        BOOST_REQUIRE(stop_cmd.has_value());
        BOOST_REQUIRE(stop_cmd->op == quic_internal::transport_command::kind::stop_sending);
        BOOST_REQUIRE_EQUAL(stop_cmd->msg.stream, 13);
        BOOST_REQUIRE_EQUAL(stop_cmd->app_error_code, 77);
        require_quic_future_exception(input_stream.input().read(), quic_error::closed);
    });
}

SEASTAR_TEST_CASE(test_quic_peer_reset_aborts_stream_input) {
    return seastar::async([] {
        auto runtime = quic_internal::make_command_runtime();
        auto engine = quic_internal::make_connection_state(runtime);

        engine->on_stream_data(14, stream_type::bidirectional, true, temporary_buffer<char>("payload", 7), false);
        auto accepted = engine->accept_stream().get();
        auto input = accepted.input();
        auto chunk = input.read().get();
        BOOST_REQUIRE_EQUAL(to_sstring(std::move(chunk)), "payload");

        auto shutdown = accepted.wait_input_shutdown();
        BOOST_REQUIRE(!shutdown.available());

        engine->on_stream_reset(14, stream_type::bidirectional, true, 1234);

        shutdown.get();
        require_quic_future_exception(input.read(), quic_error::closed);
        BOOST_REQUIRE(accepted.is_open());
        (void)accepted.output();
    });
}

SEASTAR_TEST_CASE(test_quic_peer_stop_sending_closes_stream_output) {
    return seastar::async([] {
        auto runtime = quic_internal::make_command_runtime();
        auto engine = quic_internal::make_connection_state(runtime);

        auto opened = open_stream_from_engine(runtime, engine, 16, stream_type::bidirectional);
        BOOST_REQUIRE(opened.can_write());
        (void)opened.output();

        engine->on_stream_stop_sending(
          16,
          stream_type::bidirectional,
          false,
          4321,
          quic_internal::stream_shutdown_side::write);

        require_quic_error([&opened] { (void)opened.output(); }, quic_error::closed);
        opened.close_output().get();
        BOOST_REQUIRE(!runtime->poll_command().has_value());
    });
}

SEASTAR_TEST_CASE(test_quic_peer_stop_sending_read_aborts_stream_input) {
    return seastar::async([] {
        auto runtime = quic_internal::make_command_runtime();
        auto engine = quic_internal::make_connection_state(runtime);

        engine->on_stream_data(18, stream_type::bidirectional, true, temporary_buffer<char>("payload", 7), false);
        auto accepted = engine->accept_stream().get();
        auto input = accepted.input();
        auto chunk = input.read().get();
        BOOST_REQUIRE_EQUAL(to_sstring(std::move(chunk)), "payload");

        auto shutdown = accepted.wait_input_shutdown();
        BOOST_REQUIRE(!shutdown.available());

        engine->on_stream_stop_sending(
          18,
          stream_type::bidirectional,
          true,
          5678,
          quic_internal::stream_shutdown_side::read);

        shutdown.get();
        require_quic_future_exception(input.read(), quic_error::closed);
        BOOST_REQUIRE(accepted.is_open());
        (void)accepted.output();
    });
}

SEASTAR_TEST_CASE(test_quic_connected_socket_wraps_bidirectional_stream) {
    return seastar::async([] {
        auto runtime = quic_internal::make_command_runtime();
        auto engine = quic_internal::make_connection_state(runtime);
        auto local = socket_address(ipv4_addr("127.0.0.1", 3333));
        auto peer = socket_address(ipv4_addr("127.0.0.1", 4444));
        runtime->mark_transport_ready(local, peer, "h3");

        engine->on_stream_data(15, stream_type::bidirectional, true, temporary_buffer<char>("hello", 5), false);
        auto accepted = engine->accept_stream().get();
        auto sock = to_connected_socket(std::move(accepted));

        BOOST_REQUIRE_EQUAL(sock.local_address(), local);
        BOOST_REQUIRE_EQUAL(sock.remote_address(), peer);
        BOOST_REQUIRE(sock.get_nodelay());
        BOOST_REQUIRE(!sock.get_keepalive());

        auto input = sock.input();
        auto chunk = input.read().get();
        BOOST_REQUIRE_EQUAL(to_sstring(std::move(chunk)), "hello");
        auto consume_cmd = runtime->poll_command();
        BOOST_REQUIRE(consume_cmd.has_value());
        BOOST_REQUIRE(consume_cmd->op == quic_internal::transport_command::kind::consume_stream_data);
        BOOST_REQUIRE_EQUAL(consume_cmd->msg.stream, 15);

        sock.shutdown_output();
        auto close_cmd = runtime->poll_command();
        BOOST_REQUIRE(close_cmd.has_value());
        BOOST_REQUIRE(close_cmd->op == quic_internal::transport_command::kind::send);
        BOOST_REQUIRE(close_cmd->msg.fin);

        auto shutdown = sock.wait_input_shutdown();
        BOOST_REQUIRE(!shutdown.available());
        sock.shutdown_input();
        shutdown.get();
        auto stop_cmd = runtime->poll_command();
        BOOST_REQUIRE(stop_cmd.has_value());
        BOOST_REQUIRE(stop_cmd->op == quic_internal::transport_command::kind::stop_sending);
        BOOST_REQUIRE_EQUAL(stop_cmd->msg.stream, 15);
    });
}

SEASTAR_TEST_CASE(test_quic_connection_close_terminates_pending_accept_and_open) {
    return seastar::async([] {
        auto runtime = quic_internal::make_command_runtime();
        auto engine = quic_internal::make_connection_state(runtime);

        auto pending_accept = engine->accept_stream();
        auto pending_open = engine->open_stream();
        BOOST_REQUIRE(!pending_accept.available());
        BOOST_REQUIRE(!pending_open.available());

        engine->close().get();
        BOOST_REQUIRE(!engine->is_open());

        runtime->mark_transport_closed();
        engine->on_transport_closed(std::make_exception_ptr(quic_error(quic_error::closed, "transport closed")));

        require_quic_future_exception(std::move(pending_accept), quic_error::closed);
        require_quic_future_exception(std::move(pending_open), quic_error::closed);

        auto open_cmd = runtime->poll_command();
        BOOST_REQUIRE(open_cmd.has_value());
        BOOST_REQUIRE(open_cmd->op == quic_internal::transport_command::kind::open_stream);
        BOOST_REQUIRE(!open_cmd->open_result);

        auto close_cmd = runtime->poll_command();
        BOOST_REQUIRE(close_cmd.has_value());
        BOOST_REQUIRE(close_cmd->op == quic_internal::transport_command::kind::close_connection);
    });
}

SEASTAR_TEST_CASE(test_quic_stream_closed_detaches_old_handle_from_reused_stream_id) {
    return seastar::async([] {
        auto runtime = quic_internal::make_command_runtime();
        auto engine = quic_internal::make_connection_state(runtime);

        engine->on_stream_data(22, stream_type::bidirectional, true, temporary_buffer<char>("old", 3), false);
        auto old_stream = engine->accept_stream().get();
        auto old_input = old_stream.input();
        auto old_chunk = old_input.read().get();
        BOOST_REQUIRE_EQUAL(to_sstring(std::move(old_chunk)), "old");
        auto old_consume_cmd = runtime->poll_command();
        BOOST_REQUIRE(old_consume_cmd.has_value());
        BOOST_REQUIRE(old_consume_cmd->op == quic_internal::transport_command::kind::consume_stream_data);

        engine->on_stream_closed(22);
        engine->on_stream_data(22, stream_type::bidirectional, true, temporary_buffer<char>("new", 3), true);

        auto new_stream = engine->accept_stream().get();
        BOOST_REQUIRE_EQUAL(new_stream.id(), 22);
        auto new_input = new_stream.input();
        auto new_chunk = new_input.read().get();
        BOOST_REQUIRE_EQUAL(to_sstring(std::move(new_chunk)), "new");
        BOOST_REQUIRE(new_input.read().get().empty());

        old_stream.close_output().get();
        auto close_cmd = runtime->poll_command();
        BOOST_REQUIRE(close_cmd.has_value());
        BOOST_REQUIRE(close_cmd->op == quic_internal::transport_command::kind::send);
        BOOST_REQUIRE_EQUAL(close_cmd->msg.stream, 22);
        BOOST_REQUIRE(close_cmd->msg.fin);
    });
}

SEASTAR_TEST_CASE(test_quic_transport_close_aborts_pending_accept_and_existing_streams) {
    return seastar::async([] {
        auto runtime = quic_internal::make_command_runtime();
        auto engine = quic_internal::make_connection_state(runtime);

        engine->on_stream_data(17, stream_type::bidirectional, true, temporary_buffer<char>("payload", 7), false);
        auto accepted = engine->accept_stream().get();
        auto input = accepted.input();
        auto pending_accept = engine->accept_stream();

        engine->on_transport_closed(std::make_exception_ptr(quic_error(quic_error::closed, "transport closed")));

        require_quic_future_exception(std::move(pending_accept), quic_error::closed);
        require_quic_future_exception(input.read(), quic_error::closed);
        BOOST_REQUIRE(!accepted.is_open());
    });
}

SEASTAR_TEST_CASE(test_quic_receive_budget_accepts_payload_at_limit) {
    return seastar::async([] {
        connection_options options;
        options.max_pending_receive_bytes = 4;
        auto runtime = quic_internal::make_command_runtime(options);
        auto engine = quic_internal::make_connection_state(runtime, options);

        engine->on_stream_data(20, stream_type::bidirectional, true, temporary_buffer<char>("ping", 4), false);

        BOOST_REQUIRE(!runtime->transport_failed());
        auto accepted = engine->accept_stream().get();
        auto input = accepted.input();
        auto chunk = input.read().get();
        BOOST_REQUIRE_EQUAL(to_sstring(std::move(chunk)), "ping");

        auto consume_cmd = runtime->poll_command();
        BOOST_REQUIRE(consume_cmd.has_value());
        BOOST_REQUIRE(consume_cmd->op == quic_internal::transport_command::kind::consume_stream_data);
        BOOST_REQUIRE_EQUAL(consume_cmd->msg.stream, 20);
        BOOST_REQUIRE_EQUAL(consume_cmd->consumed_bytes, 4);
    });
}

SEASTAR_TEST_CASE(test_quic_receive_budget_failure_stops_only_stream_input) {
    return seastar::async([] {
        connection_options options;
        options.max_pending_receive_bytes = 4;
        auto runtime = quic_internal::make_command_runtime(options);
        auto engine = quic_internal::make_connection_state(runtime, options);

        engine->on_stream_data(19, stream_type::bidirectional, true, temporary_buffer<char>("hello", 5), false);

        BOOST_REQUIRE(!runtime->transport_failed());
        auto accepted = engine->accept_stream().get();
        require_quic_future_exception(accepted.input().read(), quic_error::io);
        BOOST_REQUIRE(!runtime->poll_command().has_value());
    });
}

SEASTAR_TEST_CASE(test_quic_receive_flow_control_window_is_capped_by_pending_receive_budget) {
    return seastar::async([] {
        connection_options options;
        options.max_pending_receive_bytes = 4096;
        options.transport.initial_max_stream_data_bidi_local = 1 << 20;
        options.transport.initial_max_stream_data_bidi_remote = 1 << 19;
        options.transport.initial_max_stream_data_uni = 1 << 18;
        options.transport.initial_max_data = 1 << 21;

        BOOST_REQUIRE_EQUAL(
          effective_initial_receive_window(options, options.transport.initial_max_stream_data_bidi_local),
          options.max_pending_receive_bytes);
        BOOST_REQUIRE_EQUAL(
          effective_initial_receive_window(options, options.transport.initial_max_stream_data_bidi_remote),
          options.max_pending_receive_bytes);
        BOOST_REQUIRE_EQUAL(
          effective_initial_receive_window(options, options.transport.initial_max_stream_data_uni),
          options.max_pending_receive_bytes);
        BOOST_REQUIRE_EQUAL(
          effective_initial_receive_window(options, options.transport.initial_max_data),
          options.max_pending_receive_bytes);

        options.transport.initial_max_data = 2048;
        BOOST_REQUIRE_EQUAL(
          effective_initial_receive_window(options, options.transport.initial_max_data),
          options.transport.initial_max_data);
    });
}

SEASTAR_TEST_CASE(test_quic_accept_stream_queue_overflow_defers_delivery) {
    return seastar::async([] {
        auto runtime = quic_internal::make_command_runtime();
        auto engine = quic_internal::make_connection_state(runtime);

        for (stream_id sid = 0; sid < 1025; ++sid) {
            engine->on_stream_data(sid, stream_type::unidirectional, true, temporary_buffer<char>(), false);
        }

        BOOST_REQUIRE(!runtime->transport_failed());

        for (stream_id sid = 0; sid < 1025; ++sid) {
            auto accepted = engine->accept_stream().get();
            BOOST_REQUIRE_EQUAL(accepted.id(), sid);
        }
    });
}

SEASTAR_TEST_CASE(test_quic_runtime_close_and_error_drain_pending_open_streams) {
    return seastar::async([] {
        auto runtime = quic_internal::make_command_runtime();
        auto open = runtime->open_stream(stream_type::bidirectional);
        BOOST_REQUIRE(!open.available());

        runtime->mark_transport_closed();

        require_quic_future_exception(std::move(open), quic_error::closed);
        require_quic_future_exception(runtime->send(make_message(1, "x")), quic_error::closed);

        auto failed_runtime = quic_internal::make_command_runtime();
        auto failed_open = failed_runtime->open_stream(stream_type::bidirectional);
        failed_runtime->mark_error(quic_error::protocol, "bad packet");
        require_quic_future_exception(std::move(failed_open), quic_error::protocol);
        require_quic_future_exception(failed_runtime->open_stream(stream_type::bidirectional), quic_error::protocol);
    });
}

SEASTAR_TEST_CASE(test_quic_reading_stream_queues_consumed_credit) {
    return seastar::async([] {
        auto command_runtime = quic_internal::make_command_runtime();
        auto connection_state = quic_internal::make_connection_state(command_runtime);

        connection_state->on_stream_data(
          0,
          stream_type::bidirectional,
          true,
          temporary_buffer<char>("ping", 4),
          false);

        auto accepted = connection_state->accept_stream().get();
        auto input = accepted.input();
        auto chunk = input.read().get();

        BOOST_REQUIRE_EQUAL(to_sstring(std::move(chunk)), "ping");

        auto cmd = command_runtime->poll_command();
        BOOST_REQUIRE(cmd.has_value());
        BOOST_REQUIRE(cmd->op == quic_internal::transport_command::kind::consume_stream_data);
        BOOST_REQUIRE_EQUAL(cmd->msg.stream, 0);
        BOOST_REQUIRE_EQUAL(cmd->consumed_bytes, 4);
    });
}

SEASTAR_TEST_CASE(test_quic_consume_stream_command_updates_transport_credit) {
    return seastar::async([] {
        fake_connection_transport transport;
        quic_internal::transport_command cmd;
        cmd.op = quic_internal::transport_command::kind::consume_stream_data;
        cmd.msg.stream = 7;
        cmd.consumed_bytes = 64;

        quic_internal::handle_transport_command(transport, std::move(cmd)).get();

        BOOST_REQUIRE_EQUAL(transport.consume_calls, 1);
        BOOST_REQUIRE_EQUAL(transport.consumed_sid, 7);
        BOOST_REQUIRE_EQUAL(transport.consumed_len, 64);
        BOOST_REQUIRE_EQUAL(transport.write_pending_calls, 1);
        BOOST_REQUIRE_EQUAL(transport.send_datagram_calls, 0);
        BOOST_REQUIRE_EQUAL(transport.rearm_calls, 1);
        BOOST_REQUIRE(!transport.last_error.has_value());
    });
}

SEASTAR_TEST_CASE(test_quic_send_backpressure_waits_for_transport_consumption) {
    return seastar::async([] {
        connection_options options;
        options.max_pending_send_bytes = 4;

        auto command_runtime = quic_internal::make_command_runtime(options);
        command_runtime->send(make_message(0, "ping")).get();

        auto first = command_runtime->poll_command();
        BOOST_REQUIRE(first.has_value());
        BOOST_REQUIRE(first->op == quic_internal::transport_command::kind::send);

        auto blocked_send = command_runtime->send(make_message(0, "x"));
        BOOST_REQUIRE(!blocked_send.available());

        command_runtime->complete_send_bytes(first->msg.payload.size());
        blocked_send.get();
    });
}

SEASTAR_TEST_CASE(test_quic_send_backpressure_chunks_single_large_send) {
    return seastar::async([] {
        connection_options options;
        options.max_pending_send_bytes = 4;

        auto command_runtime = quic_internal::make_command_runtime(options);
        auto oversized_send = command_runtime->send(make_message(0, "hello", true));
        BOOST_REQUIRE(!oversized_send.available());

        auto first = command_runtime->poll_command();
        BOOST_REQUIRE(first.has_value());
        BOOST_REQUIRE(first->op == quic_internal::transport_command::kind::send);
        BOOST_REQUIRE_EQUAL(to_sstring(first->msg.payload.share()), "hell");
        BOOST_REQUIRE(!first->msg.fin);
        BOOST_REQUIRE(!command_runtime->poll_command().has_value());

        command_runtime->complete_send_bytes(first->msg.payload.size());
        oversized_send.get();

        auto second = command_runtime->poll_command();
        BOOST_REQUIRE(second.has_value());
        BOOST_REQUIRE(second->op == quic_internal::transport_command::kind::send);
        BOOST_REQUIRE_EQUAL(to_sstring(second->msg.payload.share()), "o");
        BOOST_REQUIRE(second->msg.fin);
        BOOST_REQUIRE(!command_runtime->poll_command().has_value());
    });
}

SEASTAR_TEST_CASE(test_quic_consume_stream_data_coalesces_pending_credit) {
    return seastar::async([] {
        auto command_runtime = quic_internal::make_command_runtime(connection_options{});

        command_runtime->consume_stream_data(7, 4);
        command_runtime->consume_stream_data(7, 5);

        auto cmd = command_runtime->poll_command();
        BOOST_REQUIRE(cmd.has_value());
        BOOST_REQUIRE(cmd->op == quic_internal::transport_command::kind::consume_stream_data);
        BOOST_REQUIRE_EQUAL(cmd->msg.stream, 7);
        BOOST_REQUIRE_EQUAL(cmd->consumed_bytes, 9);
        BOOST_REQUIRE(!command_runtime->poll_command().has_value());
    });
}

SEASTAR_TEST_CASE(test_quic_send_command_defers_remaining_payload_when_blocked) {
    return seastar::async([] {
        fake_connection_transport transport;
        transport.stream_write_results.push_back(quic_internal::transport_stream_write_result{
          .nwrite = ngtcp2_err_write_more,
          .consumed = 3,
        });
        transport.stream_write_results.push_back(quic_internal::transport_stream_write_result{
          .nwrite = 0,
          .consumed = 0,
        });

        quic_internal::transport_command cmd;
        cmd.op = quic_internal::transport_command::kind::send;
        cmd.msg = make_message(9, "hello");

        auto blocked = quic_internal::handle_transport_command(transport, std::move(cmd)).get();

        BOOST_REQUIRE(blocked.has_value());
        BOOST_REQUIRE(blocked->op == quic_internal::transport_command::kind::send);
        BOOST_REQUIRE_EQUAL(blocked->msg.stream, 9);
        BOOST_REQUIRE_EQUAL(to_sstring(blocked->msg.payload.share()), "lo");
        BOOST_REQUIRE_EQUAL(transport.completed_send_bytes, 3);
        BOOST_REQUIRE_EQUAL(transport.stream_write_calls, 2);
        BOOST_REQUIRE_EQUAL(transport.write_pending_calls, 2);
        BOOST_REQUIRE_EQUAL(transport.send_datagram_calls, 0);
        BOOST_REQUIRE_EQUAL(transport.rearm_calls, 1);
        BOOST_REQUIRE(!transport.last_error.has_value());
    });
}

SEASTAR_TEST_CASE(test_quic_send_command_retry_completes_deferred_payload) {
    return seastar::async([] {
        fake_connection_transport transport;
        transport.stream_write_results.push_back(quic_internal::transport_stream_write_result{
          .nwrite = ngtcp2_err_write_more,
          .consumed = 3,
        });
        transport.stream_write_results.push_back(quic_internal::transport_stream_write_result{
          .nwrite = 0,
          .consumed = 0,
        });

        quic_internal::transport_command first_cmd;
        first_cmd.op = quic_internal::transport_command::kind::send;
        first_cmd.msg = make_message(11, "hello");

        auto blocked = quic_internal::handle_transport_command(transport, std::move(first_cmd)).get();
        BOOST_REQUIRE(blocked.has_value());

        transport.stream_write_results.push_back(quic_internal::transport_stream_write_result{
          .nwrite = 17,
          .consumed = 2,
        });

        auto resumed = quic_internal::handle_transport_command(transport, std::move(*blocked)).get();

        BOOST_REQUIRE(!resumed.has_value());
        BOOST_REQUIRE_EQUAL(transport.completed_send_bytes, 5);
        BOOST_REQUIRE_EQUAL(transport.stream_write_calls, 3);
        BOOST_REQUIRE_EQUAL(transport.write_pending_calls, 3);
        BOOST_REQUIRE_EQUAL(transport.send_datagram_calls, 1);
        BOOST_REQUIRE_EQUAL(transport.rearm_calls, 2);
        BOOST_REQUIRE(!transport.last_error.has_value());
    });
}

SEASTAR_TEST_CASE(test_quic_send_command_defers_payload_when_stream_flow_control_blocks) {
    return seastar::async([] {
        fake_connection_transport transport;
        transport.stream_write_results.push_back(quic_internal::transport_stream_write_result{
          .nwrite = ngtcp2_err_stream_data_blocked,
          .consumed = 0,
        });

        quic_internal::transport_command cmd;
        cmd.op = quic_internal::transport_command::kind::send;
        cmd.msg = make_message(13, "hello");

        auto blocked = quic_internal::handle_transport_command(transport, std::move(cmd)).get();

        BOOST_REQUIRE(blocked.has_value());
        BOOST_REQUIRE(blocked->op == quic_internal::transport_command::kind::send);
        BOOST_REQUIRE_EQUAL(blocked->msg.stream, 13);
        BOOST_REQUIRE_EQUAL(to_sstring(blocked->msg.payload.share()), "hello");
        BOOST_REQUIRE(!blocked->msg.fin);
        BOOST_REQUIRE_EQUAL(transport.completed_send_bytes, 0);
        BOOST_REQUIRE_EQUAL(transport.stream_write_calls, 1);
        BOOST_REQUIRE_EQUAL(transport.write_pending_calls, 1);
        BOOST_REQUIRE_EQUAL(transport.send_datagram_calls, 0);
        BOOST_REQUIRE_EQUAL(transport.rearm_calls, 1);
        BOOST_REQUIRE(!transport.last_error.has_value());
    });
}

SEASTAR_TEST_CASE(test_quic_send_command_retry_fin_completes_without_duplicate_write) {
    return seastar::async([] {
        fake_connection_transport transport;
        transport.stream_write_results.push_back(quic_internal::transport_stream_write_result{
          .nwrite = ngtcp2_err_stream_data_blocked,
          .consumed = 0,
        });

        quic_internal::transport_command first_cmd;
        first_cmd.op = quic_internal::transport_command::kind::send;
        first_cmd.msg = make_message(17, "hello", true);

        auto blocked = quic_internal::handle_transport_command(transport, std::move(first_cmd)).get();
        BOOST_REQUIRE(blocked.has_value());
        BOOST_REQUIRE(blocked->msg.fin);
        BOOST_REQUIRE_EQUAL(to_sstring(blocked->msg.payload.share()), "hello");

        transport.stream_write_results.push_back(quic_internal::transport_stream_write_result{
          .nwrite = 19,
          .consumed = 5,
        });

        auto resumed = quic_internal::handle_transport_command(transport, std::move(*blocked)).get();

        BOOST_REQUIRE(!resumed.has_value());
        BOOST_REQUIRE_EQUAL(transport.completed_send_bytes, 5);
        BOOST_REQUIRE_EQUAL(transport.stream_write_calls, 2);
        BOOST_REQUIRE_EQUAL(transport.write_pending_calls, 2);
        BOOST_REQUIRE_EQUAL(transport.send_datagram_calls, 1);
        BOOST_REQUIRE_EQUAL(transport.rearm_calls, 2);
        BOOST_REQUIRE(!transport.last_error.has_value());
    });
}

SEASTAR_TEST_CASE(test_quic_open_stream_command_completes_result) {
    return seastar::async([] {
        fake_connection_transport transport;
        transport.open_stream_results.push_back(quic_internal::transport_open_stream_result{
          .rv = 0,
          .sid = 21,
        });

        auto result = std::make_shared<promise<stream_id>>();
        auto result_future = result->get_future();

        quic_internal::transport_command cmd;
        cmd.op = quic_internal::transport_command::kind::open_stream;
        cmd.type = stream_type::bidirectional;
        cmd.open_result = result;

        auto blocked = quic_internal::handle_transport_command(transport, std::move(cmd)).get();

        BOOST_REQUIRE(!blocked.has_value());
        BOOST_REQUIRE_EQUAL(result_future.get(), 21);
        BOOST_REQUIRE_EQUAL(transport.open_stream_calls, 1);
        BOOST_REQUIRE_EQUAL(transport.complete_open_stream_calls, 1);
        BOOST_REQUIRE_EQUAL(transport.completed_open_sid, 21);
        BOOST_REQUIRE_EQUAL(transport.write_pending_calls, 1);
        BOOST_REQUIRE_EQUAL(transport.rearm_calls, 1);
        BOOST_REQUIRE(!transport.last_error.has_value());
        BOOST_REQUIRE(!transport.open_stream_error.has_value());
    });
}

SEASTAR_TEST_CASE(test_quic_open_stream_command_failure_propagates_error) {
    return seastar::async([] {
        fake_connection_transport transport;
        transport.open_stream_results.push_back(quic_internal::transport_open_stream_result{
          .rv = NGTCP2_ERR_PROTO,
          .sid = invalid_stream_id,
        });

        auto result = std::make_shared<promise<stream_id>>();
        auto result_future = result->get_future();

        quic_internal::transport_command cmd;
        cmd.op = quic_internal::transport_command::kind::open_stream;
        cmd.type = stream_type::bidirectional;
        cmd.open_result = result;

        auto blocked = quic_internal::handle_transport_command(transport, std::move(cmd)).get();

        BOOST_REQUIRE(!blocked.has_value());
        BOOST_REQUIRE_EQUAL(transport.open_stream_calls, 1);
        BOOST_REQUIRE_EQUAL(transport.fail_open_stream_calls, 1);
        BOOST_REQUIRE(transport.open_stream_error.has_value());
        BOOST_REQUIRE(*transport.open_stream_error == quic_error::protocol);
        BOOST_REQUIRE(!transport.open_stream_error_detail.empty());
        BOOST_REQUIRE(transport.last_error.has_value());
        BOOST_REQUIRE(*transport.last_error == quic_error::protocol);
        BOOST_REQUIRE_EQUAL(transport.write_pending_calls, 0);
        BOOST_REQUIRE_EQUAL(transport.rearm_calls, 0);

        try {
            (void)result_future.get();
            BOOST_FAIL("expected open_stream to fail");
        } catch (const quic_error& e) {
            BOOST_REQUIRE(e.code() == quic_error::protocol);
        }
    });
}

SEASTAR_TEST_CASE(test_quic_open_stream_command_defers_when_stream_id_blocked) {
    return seastar::async([] {
        fake_connection_transport transport;
        transport.open_stream_results.push_back(quic_internal::transport_open_stream_result{
          .rv = ngtcp2_err_stream_id_blocked,
          .sid = invalid_stream_id,
        });

        auto result = std::make_shared<promise<stream_id>>();
        auto result_future = result->get_future();

        quic_internal::transport_command cmd;
        cmd.op = quic_internal::transport_command::kind::open_stream;
        cmd.type = stream_type::unidirectional;
        cmd.open_result = result;

        auto blocked = quic_internal::open_stream(transport, std::move(cmd)).get();

        BOOST_REQUIRE(blocked);
        BOOST_REQUIRE(!result_future.available());
        BOOST_REQUIRE_EQUAL(transport.open_stream_calls, 1);
        BOOST_REQUIRE_EQUAL(transport.deferred_open_stream_calls, 1);
        BOOST_REQUIRE_EQUAL(transport.deferred_uni_open_streams.size(), 1);
        BOOST_REQUIRE_EQUAL(transport.write_pending_calls, 1);
        BOOST_REQUIRE_EQUAL(transport.rearm_calls, 1);
    });
}

SEASTAR_TEST_CASE(test_quic_retry_blocked_open_streams_completes_deferred_stream) {
    return seastar::async([] {
        fake_connection_transport transport;
        transport.retry_blocked_open_streams = true;
        transport.bidi_retry_pending = true;
        transport.open_stream_results.push_back(quic_internal::transport_open_stream_result{
          .rv = 0,
          .sid = 23,
        });

        auto result = std::make_shared<promise<stream_id>>();
        auto result_future = result->get_future();

        quic_internal::transport_command cmd;
        cmd.op = quic_internal::transport_command::kind::open_stream;
        cmd.type = stream_type::bidirectional;
        cmd.open_result = result;
        transport.defer_blocked_open_stream(std::move(cmd));

        quic_internal::retry_blocked_open_streams(transport, stream_type::bidirectional).get();

        BOOST_REQUIRE(!transport.bidi_retry_pending);
        BOOST_REQUIRE_EQUAL(transport.clear_bidi_retry_calls, 1);
        BOOST_REQUIRE_EQUAL(transport.open_stream_calls, 1);
        BOOST_REQUIRE_EQUAL(transport.complete_open_stream_calls, 1);
        BOOST_REQUIRE_EQUAL(result_future.get(), 23);
        BOOST_REQUIRE(transport.deferred_bidi_open_streams.empty());
    });
}

SEASTAR_TEST_CASE(test_quic_send_command_draining_stops_transport_and_completes_bytes) {
    return seastar::async([] {
        fake_connection_transport transport;
        transport.stream_write_results.push_back(quic_internal::transport_stream_write_result{
          .nwrite = ngtcp2_err_draining,
          .consumed = 0,
        });

        auto blocked = quic_internal::send_stream_message(transport, make_message(25, "hello")).get();

        BOOST_REQUIRE(!blocked.has_value());
        BOOST_REQUIRE(!transport.active);
        BOOST_REQUIRE_EQUAL(transport.stop_transport_calls, 1);
        BOOST_REQUIRE_EQUAL(transport.completed_send_bytes, 5);
        BOOST_REQUIRE(!transport.last_error.has_value());
    });
}

SEASTAR_TEST_CASE(test_quic_send_command_stream_closed_discards_payload_without_transport_failure) {
    return seastar::async([] {
        fake_connection_transport transport;
        transport.stream_write_results.push_back(quic_internal::transport_stream_write_result{
          .nwrite = ngtcp2_err_stream_shut_wr,
          .consumed = 2,
        });

        auto blocked = quic_internal::send_stream_message(transport, make_message(27, "hello")).get();

        BOOST_REQUIRE(!blocked.has_value());
        BOOST_REQUIRE_EQUAL(transport.completed_send_bytes, 5);
        BOOST_REQUIRE_EQUAL(transport.stream_write_closed_calls, 1);
        BOOST_REQUIRE_EQUAL(transport.write_closed_sid, 27);
        BOOST_REQUIRE_EQUAL(transport.write_pending_calls, 1);
        BOOST_REQUIRE_EQUAL(transport.rearm_calls, 1);
        BOOST_REQUIRE(!transport.last_error.has_value());
    });
}

SEASTAR_TEST_CASE(test_quic_send_command_backend_error_fails_transport) {
    return seastar::async([] {
        fake_connection_transport transport;
        transport.stream_write_results.push_back(quic_internal::transport_stream_write_result{
          .nwrite = NGTCP2_ERR_CALLBACK_FAILURE,
          .consumed = 1,
        });

        auto blocked = quic_internal::send_stream_message(transport, make_message(29, "hello")).get();

        BOOST_REQUIRE(!blocked.has_value());
        BOOST_REQUIRE_EQUAL(transport.completed_send_bytes, 5);
        BOOST_REQUIRE(transport.last_error.has_value());
        BOOST_REQUIRE(*transport.last_error == quic_error::backend);
    });
}

SEASTAR_TEST_CASE(test_quic_send_command_without_connection_discards_payload) {
    return seastar::async([] {
        fake_connection_transport transport;
        transport.has_connection = false;

        auto blocked = quic_internal::send_stream_message(transport, make_message(31, "hello", true)).get();

        BOOST_REQUIRE(!blocked.has_value());
        BOOST_REQUIRE_EQUAL(transport.completed_send_bytes, 5);
        BOOST_REQUIRE_EQUAL(transport.stream_write_calls, 0);
        BOOST_REQUIRE_EQUAL(transport.write_pending_calls, 0);
    });
}

SEASTAR_TEST_CASE(test_quic_reset_stop_and_close_transport_commands) {
    return seastar::async([] {
        fake_connection_transport transport;

        quic_internal::transport_command reset_cmd;
        reset_cmd.op = quic_internal::transport_command::kind::reset_stream;
        reset_cmd.msg.stream = 33;
        reset_cmd.app_error_code = 1001;
        quic_internal::handle_transport_command(transport, std::move(reset_cmd)).get();

        BOOST_REQUIRE_EQUAL(transport.reset_stream_calls, 1);
        BOOST_REQUIRE_EQUAL(transport.reset_sid, 33);
        BOOST_REQUIRE_EQUAL(transport.reset_error, 1001);
        BOOST_REQUIRE_EQUAL(transport.write_pending_calls, 1);
        BOOST_REQUIRE_EQUAL(transport.rearm_calls, 1);

        quic_internal::transport_command stop_cmd;
        stop_cmd.op = quic_internal::transport_command::kind::stop_sending;
        stop_cmd.msg.stream = 35;
        stop_cmd.app_error_code = 1002;
        quic_internal::handle_transport_command(transport, std::move(stop_cmd)).get();

        BOOST_REQUIRE_EQUAL(transport.stop_sending_calls, 1);
        BOOST_REQUIRE_EQUAL(transport.stop_sending_sid, 35);
        BOOST_REQUIRE_EQUAL(transport.stop_sending_error, 1002);
        BOOST_REQUIRE_EQUAL(transport.write_pending_calls, 2);
        BOOST_REQUIRE_EQUAL(transport.rearm_calls, 2);

        quic_internal::transport_command close_cmd;
        close_cmd.op = quic_internal::transport_command::kind::close_connection;
        quic_internal::handle_transport_command(transport, std::move(close_cmd)).get();

        BOOST_REQUIRE_EQUAL(transport.request_close_calls, 1);
    });
}

SEASTAR_TEST_CASE(test_quic_transport_command_failures_mark_transport_error) {
    return seastar::async([] {
        fake_connection_transport consume_transport;
        consume_transport.consume_result = NGTCP2_ERR_PROTO;
        quic_internal::consume_stream_data(consume_transport, 37, 16).get();
        BOOST_REQUIRE_EQUAL(consume_transport.consume_calls, 1);
        BOOST_REQUIRE(consume_transport.last_error.has_value());
        BOOST_REQUIRE(*consume_transport.last_error == quic_error::protocol);
        BOOST_REQUIRE_EQUAL(consume_transport.write_pending_calls, 0);
        BOOST_REQUIRE_EQUAL(consume_transport.rearm_calls, 0);

        fake_connection_transport reset_transport;
        reset_transport.reset_result = NGTCP2_ERR_PROTO;
        quic_internal::reset_stream(reset_transport, 39, 1).get();
        BOOST_REQUIRE_EQUAL(reset_transport.reset_stream_calls, 1);
        BOOST_REQUIRE(reset_transport.last_error.has_value());
        BOOST_REQUIRE(*reset_transport.last_error == quic_error::protocol);

        fake_connection_transport stop_transport;
        stop_transport.stop_sending_result = NGTCP2_ERR_PROTO;
        quic_internal::stop_sending(stop_transport, 41, 1).get();
        BOOST_REQUIRE_EQUAL(stop_transport.stop_sending_calls, 1);
        BOOST_REQUIRE(stop_transport.last_error.has_value());
        BOOST_REQUIRE(*stop_transport.last_error == quic_error::protocol);
    });
}

SEASTAR_TEST_CASE(test_quic_flush_pending_packet_error_marks_transport_error) {
    return seastar::async([] {
        fake_connection_transport transport;
        transport.pending_packet_results.push_back(NGTCP2_ERR_PROTO);

        quic_internal::flush_pending_transport_packets(transport).get();

        BOOST_REQUIRE_EQUAL(transport.write_pending_calls, 1);
        BOOST_REQUIRE_EQUAL(transport.send_datagram_calls, 0);
        BOOST_REQUIRE_EQUAL(transport.stop_transport_calls, 0);
        BOOST_REQUIRE(transport.last_error.has_value());
        BOOST_REQUIRE(*transport.last_error == quic_error::protocol);
    });
}

SEASTAR_TEST_CASE(test_quic_recv_datagram_success_and_draining_paths) {
    return seastar::async([] {
        auto peer = socket_address(ipv4_addr("127.0.0.1", 5555));

        fake_connection_transport transport;
        quic_internal::recv_transport_datagram(transport, peer, temporary_buffer<char>("pkt", 3)).get();
        BOOST_REQUIRE_EQUAL(transport.read_datagram_calls, 1);
        BOOST_REQUIRE_EQUAL(transport.sync_path_calls, 1);
        BOOST_REQUIRE_EQUAL(transport.write_pending_calls, 1);
        BOOST_REQUIRE_EQUAL(transport.rearm_calls, 1);
        BOOST_REQUIRE(!transport.last_error.has_value());

        fake_connection_transport draining_transport;
        draining_transport.read_datagram_result = ngtcp2_err_draining;
        quic_internal::recv_transport_datagram(draining_transport, peer, temporary_buffer<char>("pkt", 3)).get();
        BOOST_REQUIRE_EQUAL(draining_transport.read_datagram_calls, 1);
        BOOST_REQUIRE_EQUAL(draining_transport.stop_transport_calls, 1);
        BOOST_REQUIRE(!draining_transport.active);
        BOOST_REQUIRE_EQUAL(draining_transport.sync_path_calls, 0);

        fake_connection_transport failed_transport;
        failed_transport.read_datagram_result = NGTCP2_ERR_PROTO;
        quic_internal::recv_transport_datagram(failed_transport, peer, temporary_buffer<char>("pkt", 3)).get();
        BOOST_REQUIRE_EQUAL(failed_transport.read_datagram_calls, 1);
        BOOST_REQUIRE_EQUAL(failed_transport.sync_path_calls, 0);
        BOOST_REQUIRE_EQUAL(failed_transport.write_pending_calls, 0);
        BOOST_REQUIRE_EQUAL(failed_transport.rearm_calls, 0);
        BOOST_REQUIRE(failed_transport.last_error.has_value());
        BOOST_REQUIRE(*failed_transport.last_error == quic_error::protocol);
    });
}

SEASTAR_TEST_CASE(test_quic_connection_close_not_sent_when_transport_cannot_close) {
    return seastar::async([] {
        fake_connection_transport transport;
        transport.can_close = false;

        quic_internal::send_connection_close(transport).get();

        BOOST_REQUIRE_EQUAL(transport.write_connection_close_calls, 0);
        BOOST_REQUIRE_EQUAL(transport.send_datagram_calls, 0);
        BOOST_REQUIRE(!transport.last_error.has_value());
    });
}

SEASTAR_TEST_CASE(test_quic_timer_and_connection_close_helpers) {
    return seastar::async([] {
        fake_connection_transport timer_transport;
        timer_transport.expiry_ns = 0;
        quic_internal::handle_transport_timer(timer_transport).get();
        BOOST_REQUIRE_EQUAL(timer_transport.timer_expiry_calls, 1);
        BOOST_REQUIRE_EQUAL(timer_transport.write_pending_calls, 1);
        BOOST_REQUIRE_EQUAL(timer_transport.rearm_calls, 1);

        fake_connection_transport idle_close_transport;
        idle_close_transport.expiry_ns = 0;
        idle_close_transport.timer_expiry_result = NGTCP2_ERR_IDLE_CLOSE;
        quic_internal::handle_transport_timer(idle_close_transport).get();
        BOOST_REQUIRE_EQUAL(idle_close_transport.timer_expiry_calls, 1);
        BOOST_REQUIRE_EQUAL(idle_close_transport.stop_transport_calls, 1);
        BOOST_REQUIRE(!idle_close_transport.active);

        fake_connection_transport close_transport;
        close_transport.can_close = true;
        close_transport.connection_close_result = 17;
        quic_internal::send_connection_close(close_transport).get();
        BOOST_REQUIRE_EQUAL(close_transport.write_connection_close_calls, 1);
        BOOST_REQUIRE_EQUAL(close_transport.send_datagram_calls, 1);
    });
}

SEASTAR_TEST_CASE(test_quic_error_strings_and_exception_details) {
    BOOST_REQUIRE_EQUAL(to_string(quic_error::none), "none");
    BOOST_REQUIRE_EQUAL(to_string(quic_error::invalid_argument), "invalid_argument");
    BOOST_REQUIRE_EQUAL(to_string(quic_error::invalid_state), "invalid_state");
    BOOST_REQUIRE_EQUAL(to_string(quic_error::io), "io");
    BOOST_REQUIRE_EQUAL(to_string(quic_error::timeout), "timeout");
    BOOST_REQUIRE_EQUAL(to_string(quic_error::protocol), "protocol");
    BOOST_REQUIRE_EQUAL(to_string(quic_error::closed), "closed");
    BOOST_REQUIRE_EQUAL(to_string(quic_error::unsupported), "unsupported");
    BOOST_REQUIRE_EQUAL(to_string(quic_error::internal), "internal");
    BOOST_REQUIRE_EQUAL(to_string(quic_error::backend), "backend");
    BOOST_REQUIRE_EQUAL(to_string(static_cast<quic_error_code>(999)), "unknown");

    quic_error plain(quic_error::closed);
    BOOST_REQUIRE(plain.code() == quic_error::closed);
    BOOST_REQUIRE_EQUAL(std::string(plain.what()), "closed");

    quic_error detailed(quic_error::protocol, "bad packet");
    BOOST_REQUIRE(detailed.code() == quic_error::protocol);
    BOOST_REQUIRE_EQUAL(std::string(detailed.what()), "protocol: bad packet");
    return make_ready_future<>();
}

SEASTAR_TEST_CASE(test_quic_error_classification_helpers) {
    BOOST_REQUIRE(classify_ngtcp2_error(0) == quic_error::none);
    BOOST_REQUIRE(classify_ngtcp2_error(NGTCP2_ERR_DRAINING) == quic_error::closed);
    BOOST_REQUIRE(classify_ngtcp2_error(NGTCP2_ERR_IDLE_CLOSE) == quic_error::timeout);
    BOOST_REQUIRE(classify_ngtcp2_error(NGTCP2_ERR_WRITE_MORE) == quic_error::none);
    BOOST_REQUIRE(classify_ngtcp2_error(NGTCP2_ERR_INVALID_ARGUMENT) == quic_error::invalid_argument);
    BOOST_REQUIRE(classify_ngtcp2_error(NGTCP2_ERR_PROTO) == quic_error::protocol);
    BOOST_REQUIRE(classify_ngtcp2_error(NGTCP2_ERR_CALLBACK_FAILURE) == quic_error::backend);

    BOOST_REQUIRE(ngtcp2_is_write_more(NGTCP2_ERR_WRITE_MORE));
    BOOST_REQUIRE(!ngtcp2_is_write_more(NGTCP2_ERR_PROTO));
    BOOST_REQUIRE(ngtcp2_is_draining(NGTCP2_ERR_DRAINING));
    BOOST_REQUIRE(!ngtcp2_is_draining(NGTCP2_ERR_PROTO));
    BOOST_REQUIRE(ngtcp2_is_idle_close(NGTCP2_ERR_IDLE_CLOSE));
    BOOST_REQUIRE(!ngtcp2_is_idle_close(NGTCP2_ERR_PROTO));

    BOOST_REQUIRE(classify_gnutls_error(0) == quic_error::none);
    BOOST_REQUIRE(classify_gnutls_error(GNUTLS_E_AGAIN) == quic_error::io);
    BOOST_REQUIRE(classify_gnutls_error(GNUTLS_E_INTERRUPTED) == quic_error::io);
#ifdef GNUTLS_E_TIMEDOUT
    BOOST_REQUIRE(classify_gnutls_error(GNUTLS_E_TIMEDOUT) == quic_error::timeout);
#endif
    BOOST_REQUIRE(classify_gnutls_error(GNUTLS_E_INVALID_REQUEST) == quic_error::backend);
    return make_ready_future<>();
}

SEASTAR_TEST_CASE(test_quic_socket_address_round_trip_ipv4_and_ipv6) {
    auto check_round_trip = [] (socket_address original) {
        validate_ip_socket_address(original, "test_address");

        sockaddr_storage storage{};
        socklen_t storage_len = 0;
        to_sockaddr_storage(original, storage, storage_len);

        ngtcp2_addr addr{};
        init_ngtcp2_addr(&addr, reinterpret_cast<const sockaddr*>(&storage), storage_len);

        auto converted = to_socket_address(addr);
        BOOST_REQUIRE(converted.has_value());
        BOOST_REQUIRE_EQUAL(*converted, original);
    };

    check_round_trip(socket_address(ipv4_addr("127.0.0.1", 1234)));
    check_round_trip(socket_address(ipv6_addr("::1", 5678)));

    ngtcp2_addr empty{};
    BOOST_REQUIRE(!to_socket_address(empty).has_value());

    sockaddr_storage short_storage{};
    auto* short_ipv4 = reinterpret_cast<sockaddr_in*>(&short_storage);
    short_ipv4->sin_family = AF_INET;
    ngtcp2_addr short_addr{};
    init_ngtcp2_addr(&short_addr, reinterpret_cast<const sockaddr*>(&short_storage), sizeof(sockaddr_in) - 1);
    BOOST_REQUIRE(!to_socket_address(short_addr).has_value());

    sockaddr_storage unsupported_storage{};
    auto* unsupported = reinterpret_cast<sockaddr*>(&unsupported_storage);
    unsupported->sa_family = AF_UNIX;
    ngtcp2_addr unsupported_addr{};
    init_ngtcp2_addr(&unsupported_addr, unsupported, sizeof(sockaddr));
    BOOST_REQUIRE(!to_socket_address(unsupported_addr).has_value());

    socket_address unspecified;
    require_quic_error([&] { validate_ip_socket_address(unspecified, "test_address"); }, quic_error::invalid_argument);
    require_quic_error([&] {
        sockaddr_storage storage{};
        socklen_t storage_len = 0;
        to_sockaddr_storage(unspecified, storage, storage_len);
    }, quic_error::invalid_argument);
    return make_ready_future<>();
}

SEASTAR_TEST_CASE(test_quic_linearize_packet_and_congestion_control_mapping) {
    std::array<temporary_buffer<char>, 0> empty{};
    auto empty_result = linearize_packet(std::span<temporary_buffer<char>>(empty));
    BOOST_REQUIRE(empty_result.empty());

    std::array<temporary_buffer<char>, 1> single{
      temporary_buffer<char>::copy_of(std::string_view("abc")),
    };
    auto single_result = linearize_packet(std::span<temporary_buffer<char>>(single));
    BOOST_REQUIRE_EQUAL(to_sstring(std::move(single_result)), "abc");

    std::array<temporary_buffer<char>, 3> chunks{
      temporary_buffer<char>::copy_of(std::string_view("ab")),
      temporary_buffer<char>::copy_of(std::string_view("cd")),
      temporary_buffer<char>::copy_of(std::string_view("ef")),
    };
    auto merged = linearize_packet(std::span<temporary_buffer<char>>(chunks));
    BOOST_REQUIRE_EQUAL(to_sstring(std::move(merged)), "abcdef");

    BOOST_REQUIRE(to_ngtcp2_cc_algo(congestion_control_algorithm::reno) == NGTCP2_CC_ALGO_RENO);
    BOOST_REQUIRE(to_ngtcp2_cc_algo(congestion_control_algorithm::cubic) == NGTCP2_CC_ALGO_CUBIC);
    BOOST_REQUIRE(to_ngtcp2_cc_algo(congestion_control_algorithm::bbr) == NGTCP2_CC_ALGO_BBR);
#ifdef NGTCP2_CC_ALGO_BBR2
    BOOST_REQUIRE(to_ngtcp2_cc_algo(congestion_control_algorithm::bbr2) == NGTCP2_CC_ALGO_BBR2);
#else
    BOOST_REQUIRE(to_ngtcp2_cc_algo(congestion_control_algorithm::bbr2) == NGTCP2_CC_ALGO_BBR);
#endif
    return make_ready_future<>();
}

SEASTAR_TEST_CASE(test_quic_client_stop_without_connect_is_noop) {
    return seastar::async([] {
        quic_client client;
        client.stop().get();
        client.stop().get();
    });
}

SEASTAR_TEST_CASE(test_quic_client_rejects_invalid_remote_address_family) {
    return seastar::async([] {
        quic_client client;
        quic_client_config cfg;
        cfg.remote_address = socket_address();

        require_quic_future_exception(client.connect(std::move(cfg)), quic_error::invalid_argument);
    });
}

SEASTAR_TEST_CASE(test_quic_server_stop_without_start_is_noop) {
    return seastar::async([] {
        quic_server server;
        server.stop().get();
        server.stop().get();
    });
}

SEASTAR_TEST_CASE(test_quic_server_accept_before_start_fails) {
    return seastar::async([] {
        quic_server server;
        require_quic_future_exception(server.accept(), quic_error::invalid_state);
    });
}

SEASTAR_TEST_CASE(test_quic_server_rejects_invalid_listen_address_family) {
    return seastar::async([] {
        quic_server server;
        quic_server_config cfg;
        cfg.listen_address = socket_address();
        cfg.crt_file = "unused.crt";
        cfg.key_file = "unused.key";

        require_quic_future_exception(server.start(std::move(cfg)), quic_error::invalid_argument);
    });
}
