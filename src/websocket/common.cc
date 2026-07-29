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

#include <seastar/core/future.hh>
#include <seastar/core/iostream.hh>
#include <seastar/core/loop.hh>
#include <seastar/core/temporary_buffer.hh>
#include <seastar/core/seastar.hh>
#include <seastar/websocket/common.hh>
#include <seastar/core/byteorder.hh>
#include "../core/crypto.hh"
#include <seastar/core/when_all.hh>
#include <seastar/util/assert.hh>
#include <seastar/util/defer.hh>
#include <random>
#include <seastar/websocket/parser.hh>
#include <system_error>
#include <utility>

namespace seastar::experimental::websocket {

logger websocket_logger("websocket");

template <bool is_client, bool text_frame>
future<> basic_connection<is_client, text_frame>::handle_ping(temporary_buffer<char> buff) {
    return send_data(opcodes::PONG, std::move(buff));
}

template <bool is_client, bool text_frame>
future<> basic_connection<is_client, text_frame>::handle_pong() {
    // TODO
    return make_ready_future<>();
}

static thread_local std::mt19937 masking_rng{std::random_device{}()};

static uint32_t generate_masking_key() {
    return masking_rng();
}

static void apply_mask(char* data, size_t len, uint32_t masking_key) {
    char mask_bytes[4];
    write_be<uint32_t>(mask_bytes, masking_key);
    for (size_t i = 0; i < len; ++i) {
        data[i] ^= mask_bytes[i % 4];
    }
}

template <bool is_client, bool text_frame>
future<> basic_connection<is_client, text_frame>::send_data(opcodes opcode, temporary_buffer<char> buff) {
    promise<> p;
    return std::exchange(_send_chain, p.get_future())
        .then([this, opcode, buff = std::move(buff)] () mutable -> future<> {
            return do_send(opcode, std::move(buff));
        }).then_wrapped([p = std::move(p)] (future<> f) mutable {
            if (f.failed()) {
                auto e = f.get_exception();
                p.set_exception(e);
                return make_exception_future(std::move(e));
            } else {
                p.set_value();
                return make_ready_future();
            }
        });
}

template <bool is_client, bool text_frame>
future<> basic_connection<is_client, text_frame>::do_send(opcodes opcode, temporary_buffer<char> buff) {
    char header[14] = {'\x80', 0}; // max: 2 + 8 (extended len) + 4 (mask key)
    size_t header_size = 2;

    header[0] += opcode;

    if ((126 <= buff.size()) && (buff.size() <= std::numeric_limits<uint16_t>::max())) {
        header[1] = 0x7E;
        write_be<uint16_t>(header + 2, buff.size());
        header_size += sizeof(uint16_t);
    } else if (std::numeric_limits<uint16_t>::max() < buff.size()) {
        header[1] = 0x7F;
        write_be<uint64_t>(header + 2, buff.size());
        header_size += sizeof(uint64_t);
    } else {
        header[1] = uint8_t(buff.size());
    }

    if constexpr (is_client) {
        // RFC 6455 §5.3: client frames must be masked
        header[1] |= 0x80; // set mask bit
        uint32_t masking_key = generate_masking_key();
        write_be<uint32_t>(header + header_size, masking_key);
        header_size += sizeof(uint32_t);
        apply_mask(buff.get_write(), buff.size(), masking_key);
    }

    co_await _write_buf.write(header, header_size);
    co_await _write_buf.write(std::move(buff));
    co_await _write_buf.flush();
}

template <bool is_client, bool text_frame>
void basic_connection<is_client, text_frame>::shutdown_input() {
    _fd.shutdown_input();
}

template <bool is_client, bool text_frame>
future<> basic_connection<is_client, text_frame>::close(bool send_close) {
    if (_close_future) {
        return _close_future->get_future();
    }
    if (_state == lifecycle_state::torn_down) {
        return make_ready_future<>();
    }

    if (send_close) {
        _close_future.emplace(initiate_close_handshake());
    } else {
        _close_future.emplace(tear_down_streams());
    }
    return _close_future->get_future();
}

template <bool is_client, bool text_frame>
future<> basic_connection<is_client, text_frame>::respond_to_peer_close() {
    if (_close_future) {
        return _close_future->get_future();
    }

    _state = lifecycle_state::closing_peer;
    _pending_inbound.emplace();
    _close_future.emplace(this->send_data(opcodes::CLOSE, temporary_buffer<char>()).handle_exception([] (std::exception_ptr ex) {
        websocket_logger.debug("Failed to send websocket close response: {}", ex);
    }).finally([this] () {
        return this->tear_down_streams();
    }));
    return _close_future->get_future();
}

template <bool is_client, bool text_frame>
future<> basic_connection<is_client, text_frame>::tear_down_streams() {
    if (_tear_down_future) {
        return _tear_down_future->get_future();
    }
    if (_state == lifecycle_state::torn_down) {
        return make_ready_future<>();
    }

    _state = lifecycle_state::tearing_down;
    _tear_down_future.emplace(drain_send_chain().finally([this] {
        return when_all(_read_buf.close(), _write_buf.close()).then([] (std::tuple<future<>,future<>> f) {
            std::get<0>(f).ignore_ready_future();
            std::get<1>(f).ignore_ready_future();
        });
    }).finally([this] {
        _state = lifecycle_state::torn_down;
    }));
    return _tear_down_future->get_future();
}

template <bool is_client, bool text_frame>
future<> basic_connection<is_client, text_frame>::initiate_close_handshake() {
    _state = lifecycle_state::closing_local;
    return this->send_data(opcodes::CLOSE, temporary_buffer<char>())
        .then_wrapped([this] (future<> f) mutable -> future<> {
            if (f.failed()) {
                auto ex = f.get_exception();
                websocket_logger.debug("Failed to send websocket close frame: {}", ex);
                return make_exception_future<>(ex);
            }
            if (_in_consume) {
                return make_ready_future<>();
            }
            _close_timer.emplace();
            _close_timer->set_callback([this] () { _fd.shutdown_input(); });
            _close_timer->arm(_close_timeout);
            return wait_close();
        }).finally([this] () {
            _close_timer.reset();
            return tear_down_streams();
        });
}

template <bool is_client, bool text_frame>
future<> basic_connection<is_client, text_frame>::wait_close() {
    return repeat([this] () {
        return _read_buf.consume(_websocket_parser).then([this] () mutable {
            if (_websocket_parser.eof()
                || !_websocket_parser.is_valid()
                || _websocket_parser.opcode() == opcodes::CLOSE) {
                return make_ready_future<stop_iteration>(stop_iteration::yes);
            };
            return make_ready_future<stop_iteration>(stop_iteration::no);
        }).handle_exception_type([] (const std::system_error& ex) {
            websocket_logger.debug("Websocket close wait stopped by input shutdown: {}", ex);
            return make_ready_future<stop_iteration>(stop_iteration::yes);
        }).handle_exception([] (std::exception_ptr ex) {
            websocket_logger.debug("Websocket close wait failed: {}", ex);
            return make_ready_future<stop_iteration>(stop_iteration::yes);
        });
    });
}

template <bool is_client, bool text_frame>
future<> basic_connection<is_client, text_frame>::consume() {
    if (_state != lifecycle_state::running) [[unlikely]] {
        _pending_inbound.emplace();
        return make_ready_future();
    }
    _in_consume = true;
    return _read_buf.consume(_websocket_parser).then([this] () mutable {
        if (_websocket_parser.is_valid()) {
            switch(_websocket_parser.opcode()) {
            // We do not distinguish between these 3 types.
            case opcodes::CONTINUATION:
            case opcodes::TEXT:
            case opcodes::BINARY:
                _pending_inbound = _websocket_parser.result();
                return make_ready_future();
            case opcodes::CLOSE:
                websocket_logger.debug("Received close frame.");
                // datatracker.ietf.org/doc/html/rfc6455#section-5.5.1
                return respond_to_peer_close();
            case opcodes::PING:
                websocket_logger.debug("Received ping frame.");
                return handle_ping(_websocket_parser.result());
            case opcodes::PONG:
                websocket_logger.debug("Received pong frame.");
                return handle_pong();
            default:
                SEASTAR_ASSERT(false); // parser guarantees only known opcodes reach here
                ;
            }
        } else if (_websocket_parser.eof()) {
            websocket_logger.debug("Websocket parse eof.");
            _pending_inbound.emplace();
            return close(false);
        }

        websocket_logger.debug("Websocket parse error.");
        // The parser keeps its error state. Tear the streams down before
        // reporting the parse failure so a handler that catches this exception
        // cannot re-enter the same parser error forever.
        return close(false).then([] {
            return make_exception_future(websocket::exception("Reading from socket has failed."));
        });
    }).finally([this] () { _in_consume = false; });
}


template <bool is_client, bool text_frame>
future<> basic_connection<is_client, text_frame>::drain_send_chain() {
    return std::exchange(_send_chain, make_ready_future<>())
        .handle_exception([] (std::exception_ptr ex) {
            websocket_logger.debug("Drained websocket send chain failure: {}", ex);
        });
}

std::string sha1_base64(std::string_view source) {
    auto& cp = internal::crypto::provider();
    return cp.base64_encode(cp.sha1_hash(source));
}

std::string encode_base64(std::string_view source) {
    return internal::crypto::provider().base64_encode(source);
}

template class basic_connection<true, false>;
template class basic_connection<true, true>;
template class basic_connection<false, false>;
template class basic_connection<false, true>;

}
