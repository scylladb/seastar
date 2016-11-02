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
#include "core/byteorder.hh"

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
    size_t max_memory = semaphore::max_counter(); ///< Maximum amount of memory that may be consumed by all requests
};

struct client_options {
    std::experimental::optional<net::tcp_keepalive_params> keepalive;
    compressor::factory* compressor_factory = nullptr;
    bool send_timeout_data = true;
};

struct server_options {
    compressor::factory* compressor_factory = nullptr;
};

inline
size_t
estimate_request_size(const resource_limits& lim, size_t serialized_size) {
    return lim.basic_request_size + serialized_size * lim.bloat_factor;
}

struct negotiation_frame {
    char magic[sizeof(rpc_magic) - 1];
    uint32_t len; // additional negotiation data length; multiple negotiation_frame_feature_record structs
};

enum class protocol_features : uint32_t {
    COMPRESS = 0,
    TIMEOUT = 1,
};

// internal representation of feature data
using feature_map = std::map<protocol_features, sstring>;

// An rpc signature, in the form signature<Ret (In0, In1, In2)>.
template <typename Function>
struct signature;

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
        bool _write_side_closed = false;
        protocol& _proto;
        bool _connected = false;
        promise<> _stopped;
        stats _stats;
        struct outgoing_entry {
            timer<> t;
            snd_buf buf;
            std::experimental::optional<promise<>> p = promise<>();
            cancellable* pcancel = nullptr;
            outgoing_entry(snd_buf b) : buf(std::move(b)) {}
            outgoing_entry(outgoing_entry&& o) : t(std::move(o.t)), buf(std::move(o.buf)), p(std::move(o.p)), pcancel(o.pcancel) {
                o.p = std::experimental::nullopt;
            }
            ~outgoing_entry() {
                if (p) {
                    if (pcancel) {
                        pcancel->cancel_send = std::function<void()>();
                        pcancel->send_back_pointer = nullptr;
                    }
                    p->set_value();
                }
            }
        };
        friend outgoing_entry;
        std::list<outgoing_entry> _outgoing_queue;
        condition_variable _outgoing_queue_cond;
        future<> _send_loop_stopped = make_ready_future<>();
        std::unique_ptr<compressor> _compressor;
        bool _timeout_negotiated = false;

        snd_buf compress(snd_buf buf) {
            if (_compressor) {
                buf = _compressor->compress(4, std::move(buf));
                static_assert(snd_buf::chunk_size >= 4, "send buffer chunk size is too small");
                write_le<uint32_t>(buf.front().get_write(), buf.size - 4);
                return std::move(buf);
            }
            return std::move(buf);
        }

        future<> send_buffer(snd_buf buf) {
            auto* b = boost::get<temporary_buffer<char>>(&buf.bufs);
            if (b) {
                // zero-copy write() is lacking batching so using copying write(). Refs #213.
                return _write_buf.write(b->get(), b->size());
            } else {
                return do_with(std::move(boost::get<std::vector<temporary_buffer<char>>>(buf.bufs)),
                        [this] (std::vector<temporary_buffer<char>>& ar) {
                    return do_for_each(ar.begin(), ar.end(), [this] (auto& b) {
                        // zero-copy write() is lacking batching so using copying write(). Refs #213.
                        return _write_buf.write(b.get(), b.size());
                    });
                });
            }
        }

        enum class outgoing_queue_type {
            request,
            response
        };
        template<outgoing_queue_type QueueType>
        void send_loop() {
            _send_loop_stopped = do_until([this] { return _error; }, [this] {
                return _outgoing_queue_cond.wait([this] { return !_outgoing_queue.empty(); }).then([this] {
                    // despite using wait with predicated above _outgoing_queue can still be empty here if
                    // there is only one entry on the list and its expire timer runs after wait() returned ready future,
                    // but before this continuation runs.
                    if (_outgoing_queue.empty()) {
                        return make_ready_future();
                    }
                    auto d = std::move(_outgoing_queue.front());
                    _outgoing_queue.pop_front();
                    d.t.cancel(); // cancel timeout timer
                    if (d.pcancel) {
                        d.pcancel->cancel_send = std::function<void()>(); // request is no longer cancellable
                    }
                    if (QueueType == outgoing_queue_type::request) {
                        static_assert(snd_buf::chunk_size >= 8, "send buffer chunk size is too small");
                        if (_timeout_negotiated) {
                            auto expire = d.t.get_timeout();
                            uint64_t left = 0;
                            if (expire != typename timer<>::time_point()) {
                                left = std::chrono::duration_cast<std::chrono::milliseconds>(expire - timer<>::clock::now()).count();
                            }
                            write_le<uint64_t>(d.buf.front().get_write(), left);
                        } else {
                            d.buf.front().trim_front(8);
                            d.buf.size -= 8;
                        }
                    }
                    d.buf = compress(std::move(d.buf));
                    auto f = send_buffer(std::move(d.buf)).then([this] {
                        _stats.sent_messages++;
                        return _write_buf.flush();
                    });
                    return f.finally([d = std::move(d)] {});
                });
            }).handle_exception([this] (std::exception_ptr eptr) {
                _error = true;
            }).finally([this] {
                _write_side_closed = true;
                return _write_buf.close();
            });
        }

        future<> stop_send_loop() {
            _error = true;
            // We must not call shutdown_output() concurrently with or after _write_buf.close()
            if (_connected && !_write_side_closed) {
                _outgoing_queue_cond.broken();
                _fd.shutdown_output();
            }
            return _send_loop_stopped.finally([this] {
                _outgoing_queue.clear();
            });
        }

    public:
        connection(connected_socket&& fd, protocol& proto) : _fd(std::move(fd)), _read_buf(_fd.input()), _write_buf(_fd.output()), _proto(proto), _connected(true) {}
        connection(protocol& proto) : _proto(proto) {}
        void set_socket(connected_socket&& fd) {
            if (_connected) {
                throw std::runtime_error("already connected");
            }
            _fd = std::move(fd);
            _read_buf =_fd.input();
            _write_buf = _fd.output();
            _connected = true;
        }
        future<> send_negotiation_frame(temporary_buffer<char> buf) {
            // zero-copy write() is lacking batching so using copying write(). Refs #213.
            return _write_buf.write(buf.get(), buf.size()).then([this] {
                _stats.sent_messages++;
                return _write_buf.flush();
            });
        }
        // functions below are public because they are used by external heavily templated functions
        // and I am not smart enough to know how to define them as friends
        future<> send(snd_buf buf, std::experimental::optional<steady_clock_type::time_point> timeout = {}, cancellable* cancel = nullptr) {
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
                return _outgoing_queue.back().p->get_future();
            } else {
                return make_exception_future<>(closed_error());
            }
        }
        bool error() { return _error; }
        auto& serializer() { return _proto._serializer; }
        auto& get_protocol() { return _proto; }
        future<> stop() {
            if (!_error) {
                _error = true;
                _fd.shutdown_input();
            }
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
            future<std::experimental::optional<uint64_t>, MsgType, int64_t, std::experimental::optional<rcv_buf>>
            read_request_frame(input_stream<char>& in);
            future<std::experimental::optional<uint64_t>, MsgType, int64_t, std::experimental::optional<rcv_buf>>
            read_request_frame_compressed(input_stream<char>& in);
            feature_map negotiate(feature_map requested);
            void send_loop() {
                protocol::connection::template send_loop<protocol::connection::outgoing_queue_type::response>();
            }
        public:
            connection(server& s, connected_socket&& fd, socket_address&& addr, protocol& proto);
            future<> process();
            future<> respond(int64_t msg_id, snd_buf&& data, std::experimental::optional<steady_clock_type::time_point> timeout);
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
            future<> wait_for_resources(size_t memory_consumed,  std::experimental::optional<steady_clock_type::time_point> timeout) {
                if (timeout) {
                    return _server._resources_available.wait(*timeout, memory_consumed);
                } else {
                    return _server._resources_available.wait(memory_consumed);
                }
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
        std::unordered_set<lw_shared_ptr<connection>> _conns;
        promise<> _ss_stopped;
        seastar::gate _reply_gate;
        server_options _options;
    public:
        server(protocol& proto, ipv4_addr addr, resource_limits memory_limit = resource_limits());
        server(protocol& proto, server_options opts, ipv4_addr addr, resource_limits memory_limit = resource_limits());
        server(protocol& proto, server_socket, resource_limits memory_limit = resource_limits(), server_options opts = server_options{});
        server(protocol& proto, server_options opts, server_socket, resource_limits memory_limit = resource_limits());
        void accept();
        future<> stop() {
            _ss.abort_accept();
            _ss = server_socket();
            _resources_available.broken();
            return when_all(_ss_stopped.get_future(),
                parallel_for_each(_conns, [] (lw_shared_ptr<connection> conn) {
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
        ::seastar::socket _socket;
        id_type _message_id = 1;
        struct reply_handler_base {
            timer<> t;
            cancellable* pcancel = nullptr;
            virtual void operator()(client&, id_type, rcv_buf data) = 0;
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
            virtual void operator()(client& client, id_type msg_id, rcv_buf data) override {
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
        client_options _options;
    private:
        future<> negotiate_protocol(input_stream<char>& in);
        void negotiate(feature_map server_features);
        future<int64_t, std::experimental::optional<rcv_buf>>
        read_response_frame(input_stream<char>& in);
        future<int64_t, std::experimental::optional<rcv_buf>>
        read_response_frame_compressed(input_stream<char>& in);
        void send_loop() {
            protocol::connection::template send_loop<protocol::connection::outgoing_queue_type::request>();
        }
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
    using rpc_handler = std::function<future<> (lw_shared_ptr<typename server::connection>, std::experimental::optional<steady_clock_type::time_point> timeout, int64_t msgid,
                                                rcv_buf data)>;
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
    template<typename Ret, typename... In>
    auto make_client(signature<Ret(In...)> sig, MsgType t);

    void register_receiver(MsgType t, rpc_handler&& handler) {
        _handlers.emplace(t, std::move(handler));
    }

    template <typename FrameType, typename Info>
    typename FrameType::return_type read_frame(const Info& info, input_stream<char>& in);

    template <typename FrameType, typename Info>
    typename FrameType::return_type read_frame_compressed(const Info& info, std::unique_ptr<compressor>& compressor, input_stream<char>& in);
};
}

#include "rpc_impl.hh"
