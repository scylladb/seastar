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

#include <seastar/websocket/parser.hh>
#include <seastar/core/byteorder.hh>
#include <seastar/util/assert.hh>

namespace seastar::experimental::websocket {

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
            SEASTAR_ASSERT(_buffer.length() < 2);

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

}
