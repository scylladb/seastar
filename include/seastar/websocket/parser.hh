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

#pragma once

#include <seastar/core/seastar.hh>
#include <seastar/core/iostream.hh>

namespace seastar::experimental::websocket {

/// \addtogroup websocket
/// @{

/*!
 * \brief Possible type of a websocket frame.
 */
enum opcodes {
    CONTINUATION = 0x0,
    TEXT = 0x1,
    BINARY = 0x2,
    CLOSE = 0x8,
    PING = 0x9,
    PONG = 0xA,
    INVALID = 0xFF,
};

struct frame_header {
    static constexpr uint8_t FIN = 7;
    static constexpr uint8_t RSV1 = 6;
    static constexpr uint8_t RSV2 = 5;
    static constexpr uint8_t RSV3 = 4;
    static constexpr uint8_t MASKED = 7;

    uint8_t fin : 1;
    uint8_t rsv1 : 1;
    uint8_t rsv2 : 1;
    uint8_t rsv3 : 1;
    uint8_t opcode : 4;
    uint8_t masked : 1;
    uint8_t length : 7;
    frame_header(const char* input) {
        this->fin = (input[0] >> FIN) & 1;
        this->rsv1 = (input[0] >> RSV1) & 1;
        this->rsv2 = (input[0] >> RSV2) & 1;
        this->rsv3 = (input[0] >> RSV3) & 1;
        this->opcode = input[0] & 0b1111;
        this->masked = (input[1] >> MASKED) & 1;
        this->length = (input[1] & 0b1111111);
    }
    // Returns length of the rest of the header.
    uint64_t get_rest_of_header_length() {
        size_t next_read_length = sizeof(uint32_t); // Masking key
        if (length == 126) {
            next_read_length += sizeof(uint16_t);
        } else if (length == 127) {
            next_read_length += sizeof(uint64_t);
        }
        return next_read_length;
    }
    uint8_t get_fin() {return fin;}
    uint8_t get_rsv1() {return rsv1;}
    uint8_t get_rsv2() {return rsv2;}
    uint8_t get_rsv3() {return rsv3;}
    uint8_t get_opcode() {return opcode;}
    uint8_t get_masked() {return masked;}
    uint8_t get_length() {return length;}

    bool is_opcode_known() {
        //https://datatracker.ietf.org/doc/html/rfc6455#section-5.1
        return opcode < 0xA && !(opcode < 0x8 && opcode > 0x2);
    }
};

class websocket_parser {
    enum class parsing_state : uint8_t {
        flags_and_payload_data,
        payload_length_and_mask,
        payload
    };
    enum class connection_state : uint8_t {
        valid,
        closed,
        error
    };
    using consumption_result_t = consumption_result<char>;
    using buff_t = temporary_buffer<char>;
    // What parser is currently doing.
    parsing_state _state;
    // State of connection - can be valid, closed or should be closed
    // due to error.
    connection_state _cstate;
    sstring _buffer;
    std::unique_ptr<frame_header> _header;
    uint64_t _payload_length = 0;
    uint64_t _consumed_payload_length = 0;
    uint32_t _masking_key;
    buff_t _result;

    static future<consumption_result_t> dont_stop() {
        return make_ready_future<consumption_result_t>(continue_consuming{});
    }
    static future<consumption_result_t> stop(buff_t data) {
        return make_ready_future<consumption_result_t>(stop_consuming(std::move(data)));
    }
    uint64_t remaining_payload_length() const {
        return _payload_length - _consumed_payload_length;
    }

    // Removes mask from payload given in p.
    void remove_mask(buff_t& p, size_t n) {
        char *payload = p.get_write();
        for (uint64_t i = 0, j = 0; i < n; ++i, j = (j + 1) % 4) {
            payload[i] ^= static_cast<char>(((_masking_key << (j * 8)) >> 24));
        }
    }
public:
    websocket_parser() : _state(parsing_state::flags_and_payload_data),
                         _cstate(connection_state::valid),
                         _masking_key(0) {}
    future<consumption_result_t> operator()(temporary_buffer<char> data);
    bool is_valid() { return _cstate == connection_state::valid; }
    bool eof() { return _cstate == connection_state::closed; }
    opcodes opcode() const;
    buff_t result();
};

/// @}
}
