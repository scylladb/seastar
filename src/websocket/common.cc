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
#include <seastar/websocket/common.hh>
#include <seastar/core/byteorder.hh>
#include "../core/crypto.hh"
#include <seastar/core/when_all.hh>
#include <seastar/util/assert.hh>
#include <seastar/util/defer.hh>
#include <random>
#include <seastar/websocket/parser.hh>
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
future<> basic_connection<is_client, text_frame>::drain_send_chain() {
    return std::exchange(_send_chain, make_ready_future<>())
        .handle_exception([] (std::exception_ptr ex) {
            websocket_logger.debug("Drained websocket send chain failure: {}", ex);
        });
}

template <bool is_client, bool text_frame>
future<> basic_connection<is_client, text_frame>::response_loop() {
    return do_until([this] {return _done;}, [this] {
        // FIXME: implement error handling
        return _output_buffer.pop_eventually().then([this] (
                temporary_buffer<char> buf) {
            if (!buf) {
                return make_ready_future<>();
            }
            return send_data(text_frame ? opcodes::TEXT : opcodes::BINARY, std::move(buf));
        });
    }).finally([this]() {
        return _write_buf.close();
    });
}

template <bool is_client, bool text_frame>
void basic_connection<is_client, text_frame>::shutdown_input() {
    _fd.shutdown_input();
}

template <bool is_client, bool text_frame>
future<> basic_connection<is_client, text_frame>::close(bool send_close) {
    if (_half_close) {
        return make_ready_future<>();
    }
    _half_close = true;
    return [this, send_close]() {
        if (send_close) {
            return send_data(opcodes::CLOSE, temporary_buffer<char>(0));
        } else {
            return make_ready_future<>();
        }
    }().finally([this] {
        _done = true;
        return when_all_succeed(_input.close(), _output.close()).discard_result().finally([this] {
            _fd.shutdown_output();
        });
    });
}

template <bool is_client, bool text_frame>
future<> basic_connection<is_client, text_frame>::read_one() {
    return _read_buf.consume(_websocket_parser).then([this] () mutable {
        if (_websocket_parser.is_valid()) {
            if (_half_close) {
                // When the connection enters _half_closed state, just wait for the close to complete.
                return make_ready_future();
            }
            // FIXME: implement error handling
            switch(_websocket_parser.opcode()) {
            // We do not distinguish between these 3 types.
            case opcodes::CONTINUATION:
            case opcodes::TEXT:
            case opcodes::BINARY:
                return _input_buffer.push_eventually(_websocket_parser.result());
            case opcodes::CLOSE:
                websocket_logger.debug("Received close frame.");
                // datatracker.ietf.org/doc/html/rfc6455#section-5.5.1
                return close(true);
            case opcodes::PING:
                websocket_logger.debug("Received ping frame.");
                return handle_ping(_websocket_parser.result());
            case opcodes::PONG:
                websocket_logger.debug("Received pong frame.");
                return handle_pong();
            default:
                // Invalid - do nothing.
                ;
            }
        } else if (_websocket_parser.eof()) {
            return close(false);
        }
        websocket_logger.debug("Reading from socket has failed.");
        return close(true);
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
