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

#include <ranges>
#include <system_error>
#include <seastar/core/iostream.hh>
#include <seastar/core/circular_buffer.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/core/queue.hh>
#include <seastar/core/loop.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/do_with.hh>
#include <seastar/net/stack.hh>
#include <seastar/core/sharded.hh>
#include <seastar/util/assert.hh>

namespace seastar {

struct loopback_error_injector {
    enum class error { none, one_shot, abort };
    virtual ~loopback_error_injector() {};
    virtual error server_rcv_error() { return error::none; }
    virtual error server_snd_error() { return error::none; }
    virtual error client_rcv_error() { return error::none; }
    virtual error client_snd_error() { return error::none; }
    virtual error connect_error()    { return error::none; }
    virtual std::chrono::microseconds connect_delay() { return std::chrono::microseconds(0); }
};

class loopback_buffer {
public:
    enum class type : uint8_t {
        CLIENT_TX,
        SERVER_TX
    };
private:
    bool _aborted = false;
    queue<temporary_buffer<char>> _q{1};
    loopback_error_injector* _error_injector;
    type _type;
    std::optional<promise<>> _shutdown;
public:
    loopback_buffer(loopback_error_injector* error_injection, type t) : _error_injector(error_injection), _type(t) {}
    future<> push(temporary_buffer<char>&& b) {
        if (_aborted) {
            return make_exception_future<>(std::system_error(EPIPE, std::system_category()));
        }
        if (_error_injector) {
            auto error = _type == type::CLIENT_TX ? _error_injector->client_snd_error() : _error_injector->server_snd_error();
            if (error == loopback_error_injector::error::one_shot) {
                return make_exception_future<>(std::runtime_error("test injected glitch on send"));
            }
            if (error == loopback_error_injector::error::abort) {
                abort();
                return make_exception_future<>(std::runtime_error("test injected error on send"));
            }
        }
        return _q.push_eventually(std::move(b));
    }
    future<temporary_buffer<char>> pop() {
        if (_aborted) {
            return make_exception_future<temporary_buffer<char>>(std::system_error(EPIPE, std::system_category()));
        }
        if (_error_injector) {
            auto error = _type == type::CLIENT_TX ? _error_injector->client_rcv_error() : _error_injector->server_rcv_error();
            if (error == loopback_error_injector::error::one_shot) {
                return make_exception_future<temporary_buffer<char>>(std::runtime_error("test injected glitch on receive"));
            }
            if (error == loopback_error_injector::error::abort) {
                abort();
                return make_exception_future<temporary_buffer<char>>(std::runtime_error("test injected error on receive"));
            }
        }
        return _q.pop_eventually();
    }
    void abort() noexcept {
        shutdown();
        _aborted = true;
        _q.abort(std::make_exception_ptr(std::system_error(EPIPE, std::system_category())));
    }
    void shutdown() noexcept {
        // it can be called by both -- reader and writer socket impls
        if (_shutdown.has_value()) {
            _shutdown->set_value();
            _shutdown.reset();
        }
    }

    future<> wait_input_shutdown() {
        SEASTAR_ASSERT(!_shutdown.has_value());
        _shutdown.emplace();
        return _shutdown->get_future();
    }
};

class loopback_data_sink_impl : public data_sink_impl {
    lw_shared_ptr<foreign_ptr<lw_shared_ptr<loopback_buffer>>> _buffer;
    noncopyable_function<void()> _batch_flush_error;
public:
    explicit loopback_data_sink_impl(lw_shared_ptr<foreign_ptr<lw_shared_ptr<loopback_buffer>>> buffer, noncopyable_function<void()> flush_error)
            : _buffer(buffer)
            , _batch_flush_error(std::move(flush_error))
    {
    }
    future<> put(net::packet data) override {
        return do_with(data.release(), [this] (std::vector<temporary_buffer<char>>& bufs) {
            return do_for_each(bufs, [this] (temporary_buffer<char>& buf) {
                return smp::submit_to(_buffer->get_owner_shard(), [this, b = buf.get(), s = buf.size()] {
                    return (*_buffer)->push(temporary_buffer<char>(b, s));
                });
            });
        });
    }
    future<> close() override {
        return smp::submit_to(_buffer->get_owner_shard(), [this] {
            return (*_buffer)->push({}).handle_exception_type([] (std::system_error& err) {
                if (err.code().value() != EPIPE) {
                    throw err;
                }
            });
        });
    }

    bool can_batch_flushes() const noexcept override { return true; }
    void on_batch_flush_error() noexcept override { _batch_flush_error(); }
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
                // future::get() is destructive, so we have to play these games
                // FIXME: make future::get() non-destructive
                auto&& tmp = b.get();
                _eof = tmp.empty();
                b = make_ready_future<temporary_buffer<char>>(std::move(tmp));
            }
            return std::move(b);
        });
    }
    future<> close() override {
        if (!_eof) {
            _buffer->abort();
        } else {
            _buffer->shutdown();
        }
        return make_ready_future<>();
    }
};


class loopback_connected_socket_impl : public net::connected_socket_impl {
    lw_shared_ptr<foreign_ptr<lw_shared_ptr<loopback_buffer>>> _tx;
    lw_shared_ptr<loopback_buffer> _rx;
public:
    loopback_connected_socket_impl(foreign_ptr<lw_shared_ptr<loopback_buffer>> tx, lw_shared_ptr<loopback_buffer> rx)
            : _tx(make_lw_shared(std::move(tx))), _rx(std::move(rx)) {
    }
    data_source source() override {
        return data_source(std::make_unique<loopback_data_source_impl>(_rx));
    }
    data_sink sink() override {
        return data_sink(std::make_unique<loopback_data_sink_impl>(_tx, [this] { shutdown_input(); }));
    }
    void shutdown_input() override {
        _rx->abort();
    }
    void shutdown_output() override {
        (void)smp::submit_to(_tx->get_owner_shard(), [tx = _tx] {
            (*tx)->abort();
        });
    }
    void set_nodelay(bool nodelay) override {
    }
    bool get_nodelay() const override {
        return true;
    }
    void set_keepalive(bool keepalive) override {}
    bool get_keepalive() const override {
        return false;
    }
    void set_keepalive_parameters(const net::keepalive_params&) override {}
    net::keepalive_params get_keepalive_parameters() const override {
        return net::tcp_keepalive_params {std::chrono::seconds(0), std::chrono::seconds(0), 0};
    }
    void set_sockopt(int level, int optname, const void* data, size_t len) override {
        throw std::runtime_error("Setting custom socket options is not supported for loopback");
    }
    int get_sockopt(int level, int optname, void* data, size_t len) const override {
        throw std::runtime_error("Getting custom socket options is not supported for loopback");
    }
    socket_address local_address() const noexcept override {
        // dummy
        return {};
    }
    socket_address remote_address() const noexcept override {
        // dummy
        return {};
    }
    future<> wait_input_shutdown() override {
        return _rx->wait_input_shutdown();
    }
};

class loopback_server_socket_impl : public net::server_socket_impl {
    lw_shared_ptr<queue<connected_socket>> _pending;
public:
    explicit loopback_server_socket_impl(lw_shared_ptr<queue<connected_socket>> q)
            : _pending(std::move(q)) {
    }
    future<accept_result> accept() override {
        return _pending->pop_eventually().then([] (connected_socket&& cs) {
            return make_ready_future<accept_result>(accept_result{std::move(cs), socket_address()});
        });
    }
    void abort_accept() override {
        _pending->abort(std::make_exception_ptr(std::system_error(ECONNABORTED, std::system_category())));
    }
    socket_address local_address() const override {
        // CMH dummy
        return {};
    }
};


class loopback_connection_factory {
    unsigned _shard = 0;
    unsigned _shards_count;
    unsigned _pending_capacity = 10;
    std::vector<lw_shared_ptr<queue<connected_socket>>> _pending;
public:
    explicit loopback_connection_factory(unsigned shards_count = smp::count)
            : _shards_count(shards_count)
    {
        _pending.resize(shards_count);
    }

    static loopback_connection_factory with_pending_capacity(unsigned pending_capacity, unsigned shards_count = smp::count) {
        auto lcf = loopback_connection_factory(shards_count);
        lcf._pending_capacity = pending_capacity;
        return lcf;
    }

    server_socket get_server_socket() {
       SEASTAR_ASSERT(this_shard_id() < _shards_count);
       if (!_pending[this_shard_id()]) {
           _pending[this_shard_id()] = make_lw_shared<queue<connected_socket>>(_pending_capacity);
       }
       return server_socket(std::make_unique<loopback_server_socket_impl>(_pending[this_shard_id()]));
    }
    future<> make_new_server_connection(foreign_ptr<lw_shared_ptr<loopback_buffer>> b1, lw_shared_ptr<loopback_buffer> b2) {
        SEASTAR_ASSERT(this_shard_id() < _shards_count);
        if (!_pending[this_shard_id()]) {
            _pending[this_shard_id()] = make_lw_shared<queue<connected_socket>>(_pending_capacity);
        }
        return _pending[this_shard_id()]->push_eventually(connected_socket(std::make_unique<loopback_connected_socket_impl>(std::move(b1), b2)));
    }
    connected_socket make_new_client_connection(lw_shared_ptr<loopback_buffer> b1, foreign_ptr<lw_shared_ptr<loopback_buffer>> b2) {
        return connected_socket(std::make_unique<loopback_connected_socket_impl>(std::move(b2), b1));
    }
    unsigned next_shard() {
        return _shard++ % _shards_count;
    }
    void destroy_shard(unsigned shard) {
        SEASTAR_ASSERT(shard < _shards_count);
        _pending[shard] = nullptr;
    }
    future<> destroy_all_shards() {
        return parallel_for_each(std::views::iota(0u, _shards_count), [this](shard_id shard) {
            return smp::submit_to(shard, [this] {
                destroy_shard(this_shard_id());
            });
        });
    }
};

class loopback_socket_impl : public net::socket_impl {
    loopback_connection_factory& _factory;
    loopback_error_injector* _error_injector;
    lw_shared_ptr<loopback_buffer> _b1;
    foreign_ptr<lw_shared_ptr<loopback_buffer>> _b2;
    std::optional<promise<connected_socket>> _connect_abort;
public:
    loopback_socket_impl(loopback_connection_factory& factory, loopback_error_injector* error_injector = nullptr)
            : _factory(factory), _error_injector(error_injector)
    { }
    future<connected_socket> connect(socket_address sa, socket_address local, seastar::transport proto = seastar::transport::TCP) override {
        if (_error_injector) {
            auto error = _error_injector->connect_error();
            if (error != loopback_error_injector::error::none) {
                _connect_abort.emplace();
                return _connect_abort->get_future();
            }
        }

        auto shard = _factory.next_shard();
        _b1 = make_lw_shared<loopback_buffer>(_error_injector, loopback_buffer::type::SERVER_TX);
        return smp::submit_to(shard, [this, b1 = make_foreign(_b1)] () mutable {
            auto b2 = make_lw_shared<loopback_buffer>(_error_injector, loopback_buffer::type::CLIENT_TX);
            _b2 = make_foreign(b2);
            return _factory.make_new_server_connection(std::move(b1), b2).then([b2] {
                return make_foreign(b2);
            });
        }).then([this] (foreign_ptr<lw_shared_ptr<loopback_buffer>> b2) {
            if (_error_injector) {
                auto delay = _error_injector->connect_delay();
                if (delay != std::chrono::microseconds(0)) {
                    return seastar::sleep(delay).then([this, b2 = std::move(b2)] () mutable {
                        return _factory.make_new_client_connection(_b1, std::move(b2));
                    });
                }
            }
            return make_ready_future<connected_socket>(_factory.make_new_client_connection(_b1, std::move(b2)));
        });
    }
    virtual void set_reuseaddr(bool reuseaddr) override {}
    virtual bool get_reuseaddr() const override { return false; };

    void shutdown() override {
        if (_connect_abort) {
            _connect_abort->set_exception(std::make_exception_ptr(std::system_error(ECONNABORTED, std::system_category())));
            _connect_abort = std::nullopt;
        } else {
            _b1->abort();
            (void)smp::submit_to(_b2.get_owner_shard(), [b2 = std::move(_b2)] {
                b2->abort();
            });
        }
    }
};

}
