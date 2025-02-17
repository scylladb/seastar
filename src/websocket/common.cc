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

#include <seastar/websocket/common.hh>
#include <seastar/core/byteorder.hh>
#include <seastar/core/when_all.hh>
#include <seastar/util/assert.hh>
#include <seastar/util/defer.hh>
#include <gnutls/crypto.h>
#include <gnutls/gnutls.h>

namespace seastar::experimental::websocket {

sstring magic_key_suffix = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
logger websocket_logger("websocket");

future<> connection::handle_ping() {
    // TODO
    return make_ready_future<>();
}

future<> connection::handle_pong() {
    // TODO
    return make_ready_future<>();
}

future<> connection::send_data(opcodes opcode, temporary_buffer<char>&& buff) {
    char header[10] = {'\x80', 0};
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

    scattered_message<char> msg;
    msg.append(sstring(header, header_size));
    msg.append(std::move(buff));
    return _write_buf.write(std::move(msg)).then([this] {
        return _write_buf.flush();
    });
}

future<> connection::response_loop() {
    return do_until([this] {return _done;}, [this] {
        // FIXME: implement error handling
        return _output_buffer.pop_eventually().then([this] (
                temporary_buffer<char> buf) {
            return send_data(opcodes::BINARY, std::move(buf));
        });
    }).finally([this]() {
        return _write_buf.close();
    });
}

void connection::shutdown_input() {
    _fd.shutdown_input();
}

future<> connection::close(bool send_close) {
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

future<> connection::read_one() {
    return _read_buf.consume(_websocket_parser).then([this] () mutable {
        if (_websocket_parser.is_valid()) {
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
                return handle_ping();
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
    unsigned char hash[20];
    SEASTAR_ASSERT(sizeof(hash) == gnutls_hash_get_len(GNUTLS_DIG_SHA1));
    if (int ret = gnutls_hash_fast(GNUTLS_DIG_SHA1, source.data(), source.size(), hash);
        ret != GNUTLS_E_SUCCESS) {
        throw websocket::exception(fmt::format("gnutls_hash_fast: {}", gnutls_strerror(ret)));
    }
    return encode_base64(std::string_view(reinterpret_cast<const char*>(hash), sizeof(hash)));
}

std::string encode_base64(std::string_view source) {
    gnutls_datum_t src_data{
        .data = reinterpret_cast<uint8_t*>(const_cast<char*>(source.data())),
        .size = static_cast<unsigned>(source.size())
    };
    gnutls_datum_t encoded_data;
    if (int ret = gnutls_base64_encode2(&src_data, &encoded_data); ret != GNUTLS_E_SUCCESS) {
        throw websocket::exception(fmt::format("gnutls_base64_encode2: {}", gnutls_strerror(ret)));
    }
    auto free_encoded_data = defer([&] () noexcept { gnutls_free(encoded_data.data); });
    // base64_encoded.data is "unsigned char *"
    return std::string(reinterpret_cast<const char*>(encoded_data.data), encoded_data.size);
}

}
