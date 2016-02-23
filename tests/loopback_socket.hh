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
 * Copyright (C) 2016 ScyllaDB
 */

#pragma once

#include "core/iostream.hh"
#include "core/circular_buffer.hh"
#include "core/shared_ptr.hh"
#include "core/queue.hh"
#include "core/future-util.hh"
#include "core/do_with.hh"
#include "net/stack.hh"

class loopback_buffer {
    bool _aborted = false;
    queue<temporary_buffer<char>> _q{1};
public:
    future<> push(temporary_buffer<char>&& b) {
        if (_aborted) {
            return make_exception_future<>(std::system_error(EPIPE, std::system_category()));
        }
        return _q.push_eventually(std::move(b));
    }
    future<temporary_buffer<char>> pop() {
        if (_aborted) {
            return make_exception_future<temporary_buffer<char>>(std::system_error(EPIPE, std::system_category()));
        }
        return _q.pop_eventually();
    }
    void shutdown() {
        _aborted = true;
        _q.abort(std::make_exception_ptr(std::system_error(EPIPE, std::system_category())));
    }
};

class loopback_data_sink_impl : public data_sink_impl {
    lw_shared_ptr<loopback_buffer> _buffer;
public:
    explicit loopback_data_sink_impl(lw_shared_ptr<loopback_buffer> buffer)
            : _buffer(std::move(buffer)) {
    }
    future<> put(net::packet data) override {
        return do_with(data.release(), [this] (std::vector<temporary_buffer<char>>& bufs) {
            return do_for_each(bufs, [this] (temporary_buffer<char>& buf) {
                return _buffer->push(std::move(buf));
            });
        });
    }
    future<> close() override {
        return _buffer->push({});
    }
};

class loopback_data_source_impl : public data_source_impl {
    bool _eof = false;
    lw_shared_ptr<loopback_buffer> _buffer;
public:
    explicit loopback_data_source_impl(lw_shared_ptr<loopback_buffer> buffer)
            : _buffer(std::move(buffer)) {
    }
    future<temporary_buffer<char>> get() override {
        return _buffer->pop().then_wrapped([this] (future<temporary_buffer<char>>&& b) {
            _eof = b.failed();
            if (!_eof) {
                // future::get0() is destructive, so we have to play these games
                // FIXME: make future::get0() non-destructive
                auto&& tmp = b.get0();
                _eof = tmp.empty();
                b = make_ready_future<temporary_buffer<char>>(std::move(tmp));
            }
            return std::move(b);
        });
    }
    future<> close() override {
        if (!_eof) {
            _buffer->shutdown();
        }
        return make_ready_future<>();
    }
};


class loopback_connected_socket_impl : public net::connected_socket_impl {
    lw_shared_ptr<loopback_buffer> _tx;
    lw_shared_ptr<loopback_buffer> _rx;
public:
    loopback_connected_socket_impl(lw_shared_ptr<loopback_buffer> tx, lw_shared_ptr<loopback_buffer> rx)
            : _tx(std::move(tx)), _rx(std::move(rx)) {
    }
    data_source source() override {
        return data_source(std::make_unique<loopback_data_source_impl>(_rx));
    }
    data_sink sink() override {
        return data_sink(std::make_unique<loopback_data_sink_impl>(_tx));
    }
    future<> shutdown_input() override {
        _rx->shutdown();
        return make_ready_future<>();
    }
    future<> shutdown_output() override {
        _tx->shutdown();
        return make_ready_future<>();
    }
    void set_nodelay(bool nodelay) override {
    }
    bool get_nodelay() const override {
        return true;
    }
};

class loopback_server_socket_impl : public net::server_socket_impl {
    lw_shared_ptr<queue<connected_socket>> _pending;
public:
    explicit loopback_server_socket_impl(lw_shared_ptr<queue<connected_socket>> q)
            : _pending(std::move(q)) {
    }
    future<connected_socket, socket_address> accept() override {
        return _pending->pop_eventually().then([] (connected_socket&& cs) {
            return make_ready_future<connected_socket, socket_address>(std::move(cs), socket_address());
        });
    }
    void abort_accept() override {
        _pending->abort(std::make_exception_ptr(std::system_error(ECONNABORTED, std::system_category())));
    }
};


class loopback_connection_factory {
    lw_shared_ptr<queue<connected_socket>> _pending = make_lw_shared<queue<connected_socket>>(10);
public:
    server_socket get_server_socket() {
        return server_socket(std::make_unique<loopback_server_socket_impl>(_pending));
    }
    future<connected_socket> make_new_connection() {
        auto b1 = make_lw_shared<loopback_buffer>();
        auto b2 = make_lw_shared<loopback_buffer>();
        auto c1 = connected_socket(std::make_unique<loopback_connected_socket_impl>(b1, b2));
        auto c2 = connected_socket(std::make_unique<loopback_connected_socket_impl>(b2, b1));
        return _pending->push_eventually(std::move(c1)).then([c2 = std::move(c2)] () mutable {
            return std::move(c2);
        });
    }
};
