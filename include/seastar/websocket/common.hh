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
 * Copyright 2024 ScyllaDB
 */

#pragma once

#include <chrono>
#include <optional>
#include <seastar/core/future.hh>
#include <seastar/core/loop.hh>
#include <seastar/core/lowres_clock.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/iostream.hh>
#include <seastar/core/shared_future.hh>
#include <seastar/core/temporary_buffer.hh>
#include <seastar/net/api.hh>
#include <seastar/util/log.hh>
#include <seastar/websocket/parser.hh>

namespace seastar::experimental::websocket {

/*!
 * \brief Application handler for an established WebSocket connection.
 *
 * The input stream yields decoded WebSocket message payloads and returns an
 * empty buffer on orderly teardown. Protocol parse failures are reported to the
 * current read as websocket::exception; after teardown, later reads return an
 * empty buffer.
 *
 * The output stream accepts unframed payload data. To actively close the
 * WebSocket connection normally, the handler should close the output stream.
 * If an output write or close fails, the connection's send path is no longer
 * usable and the handler should not attempt more writes.
 */
using handler_t = std::function<future<>(input_stream<char>&, output_stream<char>&)>;

class server;

/// RFC 6455 §1.3 accept-key GUID shared by server and client handshake paths.
constexpr std::string_view websocket_magic_guid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

template <bool is_client, bool text_frame>
struct basic_connection_test_accessor;

/*!
 * \brief Configuration knobs for a WebSocket connection.
 */
struct connection_options {
    /*!
     * \brief Maximum accepted payload size for incoming data frames.
     *
     * Frames whose payload exceeds this limit cause the parser to stop with an
     * error, which is reported to the current input_stream::read() as a
     * websocket::exception. The default is 16 MiB. Does not apply to control
     * frames, which are fixed to the RFC 6455 maximum of 125 bytes.
     */
    uint64_t max_payload_length = websocket_parser::default_max_payload_length;
    /*!
     * \brief Buffer size of the output_stream wrapping the connection sink.
     *
     * Sets the chunking granularity at which the handler's writes are batched
     * before being serialized as WebSocket frames. Not the size of any
     * underlying socket buffer.
     */
    size_t output_buffer_size = 8192;
    /*!
     * \brief How long to wait for the peer's CLOSE response during a locally
     *        initiated close handshake.
     *
     * On timeout, the connection's input is shut down so teardown can proceed
     * even if the peer never sends a CLOSE frame.
     */
    lowres_clock::duration close_timeout = std::chrono::milliseconds(200);
};

/// \defgroup websocket WebSocket
/// \addtogroup websocket
/// @{

/*!
 * \brief an error in handling a WebSocket connection
 */
class exception : public std::exception {
    std::string _msg;
public:
    exception(std::string_view msg) : _msg(msg) {}
    virtual const char* what() const noexcept {
        return _msg.c_str();
    }
};

/*!
 * \brief Base implementation for a WebSocket connection (client- or server-side).
 *
 * Whether the connection behaves as a client or server is selected by the
 * \c is_client template parameter.
 */
template <bool is_client = false, bool text_frame = false>
class basic_connection : public boost::intrusive::list_base_hook<> {
    template <bool test_is_client, bool test_text_frame>
    friend struct basic_connection_test_accessor;

protected:
    using buff_t = temporary_buffer<char>;
    /*!
     * \brief Connection close lifecycle.
     *
     * A running connection can move to a local close, a peer close, or direct
     * teardown. All closing states eventually move through tearing_down to
     * torn_down. Re-entrant close callers share _close_future instead of
     * starting another handshake or stream teardown.
     */
    enum class lifecycle_state {
        running,
        closing_local,
        closing_peer,
        tearing_down,
        torn_down,
    };
    /*!
     * \brief Implementation of connection's data source.
     */
    class connection_source_impl final : public data_source_impl {
        basic_connection<is_client, text_frame>& _conn;

    public:
        connection_source_impl(basic_connection<is_client, text_frame>& conn)
            : _conn(conn) {}

        virtual future<buff_t> get() override {
            return do_until([this] () { return _conn._pending_inbound.has_value(); }, [this] () {
                return _conn.consume();
            }).then([this] () {
                buff_t buff = std::move(*_conn._pending_inbound);
                _conn._pending_inbound.reset();
                return make_ready_future<buff_t>(std::move(buff));
            });
        }

        virtual future<> close() override {
            return _conn._read_buf.close();
        }
    };

    /*!
     * \brief Implementation of connection's data sink.
     */
    class connection_sink_impl final : public data_sink_impl {
        basic_connection<is_client, text_frame>& _conn;

        opcodes opcode() {
            return text_frame ? opcodes::TEXT : opcodes::BINARY;
        }
    public:
        connection_sink_impl(basic_connection<is_client, text_frame>& conn) : _conn(conn) {}

#if SEASTAR_API_LEVEL >= 9
        future<> put(std::span<temporary_buffer<char>> d) override {
            return data_sink_impl::fallback_put(d, [this] (temporary_buffer<char>&& buf) {
                return _conn.send_data(opcode(), std::move(buf));
            });
        }
#else
        virtual future<> put(net::packet d) override {
            net::fragment f = d.frag(0);
            return _conn.send_data(opcode(), temporary_buffer<char>{std::move(f.base), f.size});
        }
#endif
        virtual future<> close() override {
            // basic_connection::close() owns close idempotency; keep the sink
            // adapter stateless so source and sink close paths share one
            // connection-level state machine.
            return _conn.close();
        }
    };

    /*!
     * \brief This function processes received PING frame.
     * https://datatracker.ietf.org/doc/html/rfc6455#section-5.5.2
     */
    future<> handle_ping(temporary_buffer<char>);
    /*!
     * \brief This function processes received PONG frame.
     * https://datatracker.ietf.org/doc/html/rfc6455#section-5.5.3
     */
    future<> handle_pong();

    connected_socket _fd;
    input_stream<char> _read_buf;
    output_stream<char> _write_buf;
    std::optional<timer<lowres_clock>> _close_timer;
    lowres_clock::duration _close_timeout;
    future<> _send_chain;
    lifecycle_state _state;
    bool _in_consume = false;
    std::optional<shared_future<>> _close_future;
    std::optional<shared_future<>> _tear_down_future;

    websocket_parser _websocket_parser;
    input_stream<char> _input;
    output_stream<char> _output;
    std::optional<temporary_buffer<char>> _pending_inbound;

    sstring _subprotocol;
    handler_t _handler;
public:
    /*!
     * \param fd established socket used for communication
     * \param options connection tuning options.
     *
     * Whether the connection is client- or server-side is selected by the
     * \c is_client template parameter. Client-side connections send masked
     * frames and expect unmasked frames from the server; server-side
     * connections are the reverse.
     */
    basic_connection(connected_socket&& fd,
            connection_options options = {})
        : _fd(std::move(fd))
        , _read_buf(_fd.input())
        , _write_buf(_fd.output())
        , _close_timeout(options.close_timeout)
        , _send_chain(make_ready_future())
        , _state(lifecycle_state::running)
        , _websocket_parser(!is_client, options.max_payload_length)
        , _input(data_source{std::make_unique<connection_source_impl>(*this)})
        , _output(data_sink{std::make_unique<connection_sink_impl>(*this)}, options.output_buffer_size)
    {
    }

    /*!
     * \brief Shut down the socket input.
     */
    void shutdown_input();
    /*!
     * \brief Close this WebSocket connection.
     *
     * \param send_close If true, perform the WebSocket CLOSE handshake (send a
     *        CLOSE frame and then tear down the streams). If false, tear down
     *        the underlying streams without sending a CLOSE frame.
     * \return A future that resolves after the underlying streams have been
     *         torn down.
     *
     * Repeated close calls share the same completion future. When close(true)
     * is invoked from within input consumption (e.g. by closing the output
     * stream from inside the handler in response to a peer message), it sends
     * a CLOSE frame but does not wait for the peer's CLOSE response, to avoid
     * deadlocking against the in-progress read.
     */
    future<> close(bool send_close = true);

protected:
    /*!
     * \brief Handle a peer-initiated CLOSE frame.
     *
     * Sends the required CLOSE response and then tears down the underlying
     * streams.
     */
    future<> respond_to_peer_close();
    /*!
     * \brief Tear down the underlying input and output streams.
     *
     * Drains any in-flight sends, then closes both streams. This is the common
     * terminal cleanup path for both graceful and abortive close flows; the
     * operation is idempotent.
     */
    future<> tear_down_streams();
    /*!
     * \brief Start a locally initiated WebSocket CLOSE handshake.
     *
     * Sends a CLOSE frame, then waits for the peer's CLOSE response and tears
     * down the streams. If called from within input consumption, skips the
     * wait to avoid deadlocking against the in-progress read. A close timer
     * shuts down input if the peer does not respond within close_timeout.
     */
    future<> initiate_close_handshake();
    /*!
     * \brief Wait until a peer CLOSE frame, parser stop condition, or EOF is received.
     */
    future<> wait_close();
    /*!
     * \brief Consume the next WebSocket frame from the input stream.
     *
     * Data frames are exposed through the connection input stream. Control
     * frames are handled internally.
     */
    future<> consume();
    /*!
     * \brief Wait for any in-flight sends to complete before teardown.
     *
     * Ensures pending sends do not outlive the connection object. Exceptions
     * from the drained sends are logged and swallowed: teardown must proceed
     * regardless of a prior send failure.
     */
    future<> drain_send_chain();
    /*!
     * \brief Queue a WebSocket frame for serialized transmission.
     *
     * Frames are serialized through a send chain. Once any queued send fails,
     * the chain remains failed and later sends fail with the same exception
     * until the chain is explicitly drained during connection teardown.
     */
    future<> send_data(opcodes opcode, temporary_buffer<char> buff);
    /*!
     * \brief Pack a WebSocket frame and write it to the underlying stream.
     */
    future<> do_send(opcodes opcode, temporary_buffer<char> buff);
};

using connection = basic_connection<false, false>;

std::string sha1_base64(std::string_view source);
std::string encode_base64(std::string_view source);

extern logger websocket_logger;

/// @}
}
