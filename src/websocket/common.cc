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

#include <seastar/websocket/common.hh>
#include <seastar/core/byteorder.hh>
#include <seastar/core/when_all.hh>
#include <seastar/util/defer.hh>
#include <gnutls/crypto.h>
#include <gnutls/gnutls.h>

namespace seastar::experimental::websocket {

sstring magic_key_suffix = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
logger websocket_logger("websocket");

opcodes websocket_parser::opcode() const {
    if (_header) {
        return opcodes(_header->opcode);
    } else {
        return opcodes::INVALID;
    }
}

websocket_parser::buff_t websocket_parser::result() {
    return std::move(_result);
}

future<websocket_parser::consumption_result_t> websocket_parser::operator()(
        temporary_buffer<char> data) {
    if (data.size() == 0) {
        // EOF
        _cstate = connection_state::closed;
        return websocket_parser::stop(std::move(data));
    }
    if (_state == parsing_state::flags_and_payload_data) {
        if (_buffer.length() + data.size() >= 2) {
            // _buffer.length() is less than 2 when entering this if body due to how
            // the rest of code is structured. The else branch will never increase
            // _buffer.length() to >=2 and other paths to this condition will always
            // have buffer cleared.
            assert(_buffer.length() < 2);

            size_t hlen = _buffer.length();
            _buffer.append(data.get(), 2 - hlen);
            data.trim_front(2 - hlen);
            _header = std::make_unique<frame_header>(_buffer.data());
            _buffer = {};

            // https://datatracker.ietf.org/doc/html/rfc6455#section-5.1
            // We must close the connection if data isn't masked.
            if ((!_header->masked) ||
                // RSVX must be 0
                (_header->rsv1 | _header->rsv2 | _header->rsv3) ||
                // Opcode must be known.
                (!_header->is_opcode_known())) {
                _cstate = connection_state::error;
                return websocket_parser::stop(std::move(data));
            }
            _state = parsing_state::payload_length_and_mask;
        } else {
            _buffer.append(data.get(), data.size());
            return websocket_parser::dont_stop();
        }
    }
    if (_state == parsing_state::payload_length_and_mask) {
        size_t const required_bytes = _header->get_rest_of_header_length();
        if (_buffer.length() + data.size() >= required_bytes) {
            if (_buffer.length() < required_bytes) {
                size_t hlen = _buffer.length();
                _buffer.append(data.get(), required_bytes - hlen);
                data.trim_front(required_bytes - hlen);
            }
            _payload_length = _header->length;
            char const *input = _buffer.data();
            if (_header->length == 126) {
                _payload_length = consume_be<uint16_t>(input);
            } else if (_header->length == 127) {
                _payload_length = consume_be<uint64_t>(input);
            }

            _masking_key = consume_be<uint32_t>(input);
            _buffer = {};
            _state = parsing_state::payload;
        } else {
            _buffer.append(data.get(), data.size());
            return websocket_parser::dont_stop();
        }
    }
    if (_state == parsing_state::payload) {
        if (data.size() < remaining_payload_length()) {
            // data has insufficient data to complete the frame - consume data.size() bytes
            if (_result.empty()) {
                _result = temporary_buffer<char>(remaining_payload_length());
                _consumed_payload_length = 0;
            }
            std::copy(data.begin(), data.end(), _result.get_write() + _consumed_payload_length);
            _consumed_payload_length += data.size();
            return websocket_parser::dont_stop();
        } else {
            // data has sufficient data to complete the frame - consume remaining_payload_length()
            auto consumed_bytes = remaining_payload_length();
            if (_result.empty()) {
                // Try to avoid memory copies in case when network packets contain one or more full
                // websocket frames.
                if (consumed_bytes == data.size()) {
                    _result = std::move(data);
                    data = temporary_buffer<char>(0);
                } else {
                    _result = data.share();
                    _result.trim(consumed_bytes);
                    data.trim_front(consumed_bytes);
                }
            } else {
                std::copy(data.begin(), data.begin() + consumed_bytes,
                          _result.get_write() + _consumed_payload_length);
                data.trim_front(consumed_bytes);
            }
            remove_mask(_result, _payload_length);
            _consumed_payload_length = 0;
            _state = parsing_state::flags_and_payload_data;
            return websocket_parser::stop(std::move(data));
        }
    }
    _cstate = connection_state::error;
    return websocket_parser::stop(std::move(data));
}

future<> connection::handle_ping() {
    // TODO
    return make_ready_future<>();
}

future<> connection::handle_pong() {
    // TODO
    return make_ready_future<>();
}

future<> connection::send_data(opcodes opcode, temporary_buffer<char>&& buff) {
    // Maximum length of header is 14:
    //  2 for static part of the header
    //  8 for payload length field at maximum size
    //  4 for optional mask
    char header[14] = {'\x80', 0};
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

    temporary_buffer<char> write_buf;
    if (_is_client) {
        header[1] |= 0x80;
        // https://datatracker.ietf.org/doc/html/rfc6455#section-5.3 requires that the masking key
        // must be unpredictable and derived from a strong source of entropy. This requirement
        // arose due to usage of WebSocket protocol in the browsers, where both server and client
        // payload generation code may be malicious and the only trusted piece is the browser
        // itself. Consequently there was a need to make the bytes on the wire unpredictable for the
        // client code, so that it cannot run attacks against intermediate proxies that do not
        // understand WebSocket.
        //
        // In the case of Seastar there is no security boundary between payload generator and
        // payload serializer in this class, accordingly in terms of security impact it is
        // sufficient to simply use predictable masking key. Zero is chosen because it does not
        // change the payload.
        uint32_t masking_key = 0;
        write_be<uint32_t>(header + header_size, masking_key);
        header_size += sizeof(uint32_t);
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
    assert(sizeof(hash) == gnutls_hash_get_len(GNUTLS_DIG_SHA1));
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
