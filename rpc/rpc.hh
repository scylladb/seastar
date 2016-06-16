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
 * Copyright (C) 2015 Cloudius Systems, Ltd.
 */

#pragma once

#include <unordered_map>
#include <unordered_set>
#include <list>
#include "core/future.hh"
#include "net/api.hh"
#include "core/reactor.hh"
#include "core/iostream.hh"
#include "core/shared_ptr.hh"
#include "core/condition-variable.hh"
#include "core/gate.hh"
#include "rpc/rpc_types.hh"

namespace rpc {

using id_type = int64_t;

struct SerializerConcept {
    // For each serializable type T, implement
    class T;
    template <typename Output>
    friend void write(const SerializerConcept&, Output& output, const T& data);
    template <typename Input>
    friend T read(const SerializerConcept&, Input& input, type<T> type_tag);  // type_tag used to disambiguate
    // Input and Output expose void read(char*, size_t) and write(const char*, size_t).
};

static constexpr char rpc_magic[] = "SSTARRPC";


/// \brief Resource limits for an RPC server
///
/// A request's memory use will be estimated as
///
///     req_mem = basic_request_size * sizeof(serialized_request) * bloat_factor
///
/// Concurrent requests will be limited so that
///
///     sum(req_mem) <= max_memory
///
/// \see server
struct resource_limits {
    size_t basic_request_size = 0; ///< Minimum request footprint in memory
    unsigned bloat_factor = 1;     ///< Serialized size multiplied by this to estimate memory used by request
    size_t max_memory = std::numeric_limits<size_t>::max(); ///< Maximum amount of memory that may be consumed by all requests
};

struct client_options {
    std::experimental::optional<net::tcp_keepalive_params> keepalive;
};

inline
size_t
estimate_request_size(const resource_limits& lim, size_t serialized_size) {
    return lim.basic_request_size + serialized_size * lim.bloat_factor;
}

struct negotiation_frame {
    char magic[sizeof(rpc_magic) - 1];
    uint32_t len; // additional negotiation data length; multiple negotiation_frame_feature_record structs
}  __attribute__((packed));

// internal representation of feature data
using feature_map = std::map<uint32_t, sstring>;

// MsgType is a type that holds type of a message. The type should be hashable
// and serializable. It is preferable to use enum for message types, but
// do not forget to provide hash function for it
template<typename Serializer, typename MsgType = uint32_t>
class protocol {
    class connection {
    protected:
        connected_socket _fd;
        input_stream<char> _read_buf;
        output_stream<char> _write_buf;
        bool _error = false;
        protocol& _proto;
        promise<> _stopped;
        stats _stats;
        struct outgoing_entry {
            timer<> t;
            sstring buf;
            promise<> p;
            cancellable* pcancel = nullptr;
            outgoing_entry(sstring b) : buf(std::move(b)) {}
            outgoing_entry(outgoing_entry&&) = default;
            ~outgoing_entry() {
                if (!buf.empty()) {
                    if (pcancel) {
                        pcancel->cancel_send = std::function<void()>();
                        pcancel->send_back_pointer = nullptr;
                    }
                    p.set_value();
                }
            }
        };
        friend outgoing_entry;
        std::list<outgoing_entry> _outgoing_queue;
        condition_variable _outgoing_queue_cond;
        future<> _send_loop_stopped = make_ready_future<>();

        void send_loop() {
            _send_loop_stopped = do_until([this] { return _error; }, [this] {
                return _outgoing_queue_cond.wait([this] { return !_outgoing_queue.empty(); }).then([this] {
                    auto d = std::move(_outgoing_queue.front());
                    _outgoing_queue.pop_front();
                    d.t.cancel(); // cancel timeout timer
                    if (d.pcancel) {
                        d.pcancel->cancel_send = std::function<void()>(); // request is no longer cancellable
                    }
                    auto f = _write_buf.write(d.buf).then([this] {
                        _stats.sent_messages++;
                        return _write_buf.flush();
                    });
                    return f.finally([d = std::move(d)] {});
                });
            }).handle_exception([this] (std::exception_ptr eptr) {
                _error = true;
            }).finally([this] {
                return _write_buf.close();
            });
        }

        future<> stop_send_loop() {
            _error = true;
            _outgoing_queue_cond.broken();
            return _send_loop_stopped.finally([this] {
                _outgoing_queue.clear();
            });
        }

    public:
        connection(connected_socket&& fd, protocol& proto) : _fd(std::move(fd)), _read_buf(_fd.input()), _write_buf(_fd.output()), _proto(proto) {}
        connection(protocol& proto) : _proto(proto) {}
        // functions below are public because they are used by external heavily templated functions
        // and I am not smart enough to know how to define them as friends
        future<> send(sstring buf, std::experimental::optional<steady_clock_type::time_point> timeout = {}, cancellable* cancel = nullptr) {
            if (!_error) {
                _outgoing_queue.emplace_back(std::move(buf));
                auto deleter = [this, it = std::prev(_outgoing_queue.cend())] {
                    _outgoing_queue.erase(it);
                };
                if (timeout) {
                    auto& t = _outgoing_queue.back().t;
                    t.set_callback(deleter);
                    t.arm(timeout.value());
                }
                if (cancel) {
                    cancel->cancel_send = std::move(deleter);
                    cancel->send_back_pointer = &_outgoing_queue.back().pcancel;
                    _outgoing_queue.back().pcancel = cancel;
                }
                _outgoing_queue_cond.signal();
                return _outgoing_queue.back().p.get_future();
            } else {
                return make_exception_future<>(closed_error());
            }
        }
        bool error() { return _error; }
        auto& serializer() { return _proto._serializer; }
        auto& get_protocol() { return _proto; }
        future<> stop() {
            _fd.shutdown_input();
            _fd.shutdown_output();
            return _stopped.get_future();
        }
    };
    friend connection;

public:
    class server {
    public:
        class connection : public protocol::connection, public enable_lw_shared_from_this<connection> {
            server& _server;
            client_info _info;
        private:
            future<> negotiate_protocol(input_stream<char>& in);
            future<MsgType, int64_t, std::experimental::optional<temporary_buffer<char>>>
            read_request_frame(input_stream<char>& in);
            feature_map negotiate(feature_map requested);
        public:
            connection(server& s, connected_socket&& fd, socket_address&& addr, protocol& proto);
            future<> process();
            future<> respond(int64_t msg_id, sstring&& data);
            client_info& info() { return _info; }
            const client_info& info() const { return _info; }
            stats get_stats() const {
                stats res = this->_stats;
                res.pending = this->_outgoing_queue.size();
                return res;
            }

            stats& get_stats_internal() {
                return this->_stats;
            }
            ipv4_addr peer_address() const {
                return ipv4_addr(_info.addr);
            }
            future<> wait_for_resources(size_t memory_consumed) {
                return _server._resources_available.wait(memory_consumed);
            }
            void release_resources(size_t memory_consumed) {
                _server._resources_available.signal(memory_consumed);
            }
            size_t estimate_request_size(size_t serialized_size) {
                return rpc::estimate_request_size(_server._limits, serialized_size);
            }
            server& get_server() {
                return _server;
            }
        };
    private:
        protocol& _proto;
        server_socket _ss;
        resource_limits _limits;
        semaphore _resources_available;
        std::unordered_set<connection*> _conns;
        bool _stopping = false;
        promise<> _ss_stopped;
        seastar::gate _reply_gate;
    public:
        server(protocol& proto, ipv4_addr addr, resource_limits memory_limit = resource_limits());
        server(protocol& proto, server_socket, resource_limits memory_limit = resource_limits());
        void accept();
        future<> stop() {
            _stopping = true; // prevents closed connections to be deleted from _conns
            _ss.abort_accept();
            _ss = server_socket();
            _resources_available.broken();
            return when_all(_ss_stopped.get_future(),
                parallel_for_each(_conns, [] (connection* conn) {
                    return conn->stop();
                }),
                _reply_gate.close()
            ).discard_result();
        }
        template<typename Func>
        void foreach_connection(Func&& f) {
            for (auto c : _conns) {
                f(*c);
            }
        }
        seastar::gate& reply_gate() {
            return _reply_gate;
        }
        friend connection;
    };

    class client : public protocol::connection {
        bool _connected = false;
        ::seastar::socket _socket;
        id_type _message_id = 1;
        struct reply_handler_base {
            timer<> t;
            cancellable* pcancel = nullptr;
            virtual void operator()(client&, id_type, temporary_buffer<char> data) = 0;
            virtual void timeout() {}
            virtual void cancel() {}
            virtual ~reply_handler_base() {
                if (pcancel) {
                    pcancel->cancel_wait = std::function<void()>();
                    pcancel->wait_back_pointer = nullptr;
                }
            };
        };
    public:
        template<typename Reply, typename Func>
        struct reply_handler final : reply_handler_base {
            Func func;
            Reply reply;
            reply_handler(Func&& f) : func(std::move(f)) {}
            virtual void operator()(client& client, id_type msg_id, temporary_buffer<char> data) override {
                return func(reply, client, msg_id, std::move(data));
            }
            virtual void timeout() override {
                reply.done = true;
                reply.p.set_exception(timeout_error());
            }
            virtual void cancel() override {
                reply.done = true;
                reply.p.set_exception(canceled_error());
            }
            virtual ~reply_handler() {}
        };
    private:
        std::unordered_map<id_type, std::unique_ptr<reply_handler_base>> _outstanding;
        ipv4_addr _server_addr;
    private:
        future<> negotiate_protocol(input_stream<char>& in);
        void negotiate(feature_map server_features);
        future<int64_t, std::experimental::optional<temporary_buffer<char>>>
        read_response_frame(input_stream<char>& in);
    public:
        /**
         * Create client object which will attempt to connect to the remote address.
         *
         * @param addr the remote address identifying this client
         * @param local the local address of this client
         */
        client(protocol& proto, ipv4_addr addr, ipv4_addr local = ipv4_addr());
        client(protocol& proto, client_options options, ipv4_addr addr, ipv4_addr local = ipv4_addr());

         /**
         * Create client object which will attempt to connect to the remote address using the
         * specified seastar::socket.
         *
         * @param addr the remote address identifying this client
         * @param local the local address of this client
         * @param socket the socket object use to connect to the remote address
         */
        client(protocol& proto, seastar::socket socket, ipv4_addr addr, ipv4_addr local = ipv4_addr());
        client(protocol& proto, client_options options, seastar::socket socket, ipv4_addr addr, ipv4_addr local = ipv4_addr());

        stats get_stats() const {
            stats res = this->_stats;
            res.wait_reply = _outstanding.size();
            res.pending = this->_outgoing_queue.size();
            return res;
        }

        stats& get_stats_internal() {
            return this->_stats;
        }
        auto next_message_id() { return _message_id++; }
        void wait_for_reply(id_type id, std::unique_ptr<reply_handler_base>&& h, std::experimental::optional<steady_clock_type::time_point> timeout, cancellable* cancel) {
            if (timeout) {
                h->t.set_callback(std::bind(std::mem_fn(&client::wait_timed_out), this, id));
                h->t.arm(timeout.value());
            }
            if (cancel) {
                cancel->cancel_wait = [this, id] {
                    _outstanding[id]->cancel();
                    _outstanding.erase(id);
                };
                h->pcancel = cancel;
                cancel->wait_back_pointer = &h->pcancel;
            }
            _outstanding.emplace(id, std::move(h));
        }
        void wait_timed_out(id_type id) {
            this->_stats.timeout++;
            _outstanding[id]->timeout();
            _outstanding.erase(id);
        }

        future<> stop() {
            if (!this->_error) {
                this->_error = true;
                _socket.shutdown();
            }
            return this->_stopped.get_future();
        }
        ipv4_addr peer_address() const {
            return _server_addr;
        }
    };
    friend server;
private:
    using rpc_handler = std::function<future<> (lw_shared_ptr<typename server::connection>, int64_t msgid,
                                                temporary_buffer<char> data)>;
    std::unordered_map<MsgType, rpc_handler> _handlers;
    Serializer _serializer;
    std::function<void(const sstring&)> _logger;
public:
    protocol(Serializer&& serializer) : _serializer(std::forward<Serializer>(serializer)) {}
    template<typename Func>
    auto make_client(MsgType t);

    // returns a function which type depends on Func
    // if Func == Ret(Args...) then return function is
    // future<Ret>(protocol::client&, Args...)
    template<typename Func>
    auto register_handler(MsgType t, Func&& func);

    void unregister_handler(MsgType t) {
        _handlers.erase(t);
    }

    void set_logger(std::function<void(const sstring&)> logger) {
        _logger = logger;
    }

    void log(const sstring& str) {
        if (_logger) {
            _logger(str);
        }
    }

    void log(const client_info& info, id_type msg_id, const sstring& str) {
        log(to_sstring("client ") + inet_ntoa(info.addr.as_posix_sockaddr_in().sin_addr) + " msg_id " + to_sstring(msg_id) + ": " + str);
    }
    void log(const client_info& info, const sstring& str) {
        log(to_sstring("client ") + inet_ntoa(info.addr.as_posix_sockaddr_in().sin_addr) + ": " + str);
    }
    void log(ipv4_addr addr, const sstring& str) {
        log(to_sstring("client ") + inet_ntoa(in_addr{net::ntoh(addr.ip)}) + ": " + str);
    }

private:
    void register_receiver(MsgType t, rpc_handler&& handler) {
        _handlers.emplace(t, std::move(handler));
    }
};
}

#include "rpc_impl.hh"
