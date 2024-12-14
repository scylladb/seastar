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

#include <seastar/core/seastar.hh>
#include <seastar/core/iostream.hh>
#include <seastar/core/queue.hh>
#include <seastar/net/api.hh>
#include <seastar/util/log.hh>
#include <seastar/websocket/parser.hh>

namespace seastar::experimental::websocket {

extern sstring magic_key_suffix;

using handler_t = std::function<future<>(input_stream<char>&, output_stream<char>&)>;

class server;

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
 * \brief a server WebSocket connection
 */
class connection : public boost::intrusive::list_base_hook<> {
protected:
    using buff_t = temporary_buffer<char>;

    /*!
     * \brief Implementation of connection's data source.
     */
    class connection_source_impl final : public data_source_impl {
        queue<buff_t>* data;

    public:
        connection_source_impl(queue<buff_t>* data) : data(data) {}

        virtual future<buff_t> get() override {
            return data->pop_eventually().then_wrapped([](future<buff_t> f){
                try {
                    return make_ready_future<buff_t>(std::move(f.get()));
                } catch(...) {
                    return current_exception_as_future<buff_t>();
                }
            });
        }

        virtual future<> close() override {
            data->push(buff_t(0));
            return make_ready_future<>();
        }
    };

    /*!
     * \brief Implementation of connection's data sink.
     */
    class connection_sink_impl final : public data_sink_impl {
        queue<buff_t>* data;
    public:
        connection_sink_impl(queue<buff_t>* data) : data(data) {}

        virtual future<> put(net::packet d) override {
            net::fragment f = d.frag(0);
            return data->push_eventually(temporary_buffer<char>{std::move(f.base), f.size});
        }

        size_t buffer_size() const noexcept override {
            return data->max_size();
        }

        virtual future<> close() override {
            data->push(buff_t(0));
            return make_ready_future<>();
        }
    };

    /*!
     * \brief This function processess received PING frame.
     * https://datatracker.ietf.org/doc/html/rfc6455#section-5.5.2
     */
    future<> handle_ping();
    /*!
     * \brief This function processess received PONG frame.
     * https://datatracker.ietf.org/doc/html/rfc6455#section-5.5.3
     */
    future<> handle_pong();

    static const size_t PIPE_SIZE = 512;
    connected_socket _fd;
    input_stream<char> _read_buf;
    output_stream<char> _write_buf;
    bool _done = false;

    websocket_parser _websocket_parser;
    queue <temporary_buffer<char>> _input_buffer;
    input_stream<char> _input;
    queue <temporary_buffer<char>> _output_buffer;
    output_stream<char> _output;

    sstring _subprotocol;
    handler_t _handler;
public:
    /*!
     * \param fd established socket used for communication
     */
    connection(connected_socket&& fd)
        : _fd(std::move(fd))
        , _read_buf(_fd.input())
        , _write_buf(_fd.output())
        , _input_buffer{PIPE_SIZE}
        , _output_buffer{PIPE_SIZE}
    {
        _input = input_stream<char>{data_source{
                std::make_unique<connection_source_impl>(&_input_buffer)}};
        _output = output_stream<char>{data_sink{
                std::make_unique<connection_sink_impl>(&_output_buffer)}};
    }

    /*!
     * \brief close the socket
     */
    void shutdown_input();
    future<> close(bool send_close = true);

protected:
    future<> read_one();
    future<> response_loop();
    /*!
     * \brief Packs buff in websocket frame and sends it to the client.
     */
    future<> send_data(opcodes opcode, temporary_buffer<char>&& buff);
};

std::string sha1_base64(std::string_view source);
std::string encode_base64(std::string_view source);

extern logger websocket_logger;

/// @}
}
