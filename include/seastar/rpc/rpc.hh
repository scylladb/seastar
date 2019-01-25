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
#include <seastar/core/future.hh>
#include <seastar/net/api.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/iostream.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/core/condition-variable.hh>
#include <seastar/core/gate.hh>
#include <seastar/rpc/rpc_types.hh>
#include <seastar/core/byteorder.hh>
#include <seastar/core/shared_future.hh>
#include <seastar/core/queue.hh>
#include <seastar/core/weak_ptr.hh>
#include <seastar/core/scheduling.hh>

namespace seastar {

namespace rpc {

using id_type = int64_t;

using rpc_semaphore = basic_semaphore<semaphore_default_exception_factory, rpc_clock_type>;
using resource_permit = semaphore_units<semaphore_default_exception_factory, rpc_clock_type>;

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

/// Specifies resource isolation for a connection.
struct isolation_config {
    /// Specifies a scheduling group under which the connection (and all its
    /// verb handlers) will execute.
    scheduling_group sched_group = current_scheduling_group();
};

/// Default isolation configuration - run everything in the default scheduling group.
isolation_config default_isolate_connection(sstring isolation_cookie);

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
    size_t max_memory = rpc_semaphore::max_counter(); ///< Maximum amount of memory that may be consumed by all requests
    /// Configures isolation for a connection based on its isolation cookie. May throw,
    /// in which case the connection will be terminated.
    std::function<isolation_config (sstring isolation_cookie)> isolate_connection = default_isolate_connection;
};

struct client_options {
    compat::optional<net::tcp_keepalive_params> keepalive;
    bool tcp_nodelay = true;
    compressor::factory* compressor_factory = nullptr;
    bool send_timeout_data = true;
    connection_id stream_parent = invalid_connection_id;
    /// Configures how this connection is isolated from other connection on the same server.
    ///
    /// \see resource_limits::isolate_connection
    sstring isolation_cookie;
};

// RPC call that passes stream connection id as a parameter
// may arrive to a different shard from where the stream connection
// was opened, so the connection id is not known to a server that handles
// the RPC call. The shard that the stream connection belong to is know
// since it is a part of connection id, but this is not enough to locate
// a server instance the connection belongs to if there are more than one
// server on the shard. Stream domain parameter is here to help with that.
// Different servers on all shards logically belonging to the same service should
// belong to the same streaming domain. Only one server on each shard can belong to
// a particulr streaming domain.
class streaming_domain_type {
    uint64_t _id;
public:
    explicit streaming_domain_type(uint64_t id) : _id(id) {}
    bool operator==(const streaming_domain_type& o) const {
        return _id == o._id;
    }
    friend struct std::hash<streaming_domain_type>;
    friend std::ostream& operator<<(std::ostream&, const streaming_domain_type&);
};

struct server_options {
    compressor::factory* compressor_factory = nullptr;
    bool tcp_nodelay = true;
    compat::optional<streaming_domain_type> streaming_domain;
    server_socket::load_balancing_algorithm load_balancing_algorithm = server_socket::load_balancing_algorithm::default_;
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
    CONNECTION_ID = 2,
    STREAM_PARENT = 3,
    ISOLATION = 4,
};

// internal representation of feature data
using feature_map = std::map<protocol_features, sstring>;

// An rpc signature, in the form signature<Ret (In0, In1, In2)>.
template <typename Function>
struct signature;

class logger {
    std::function<void(const sstring&)> _logger;

    void log(const sstring& str) const {
        if (_logger) {
            _logger(str);
        }
    }

public:
    void set(std::function<void(const sstring&)> l) {
        _logger = std::move(l);
    }

    void operator()(const client_info& info, id_type msg_id, const sstring& str) const {
        log(to_sstring("client ") + inet_ntoa(info.addr.as_posix_sockaddr_in().sin_addr) + " msg_id " + to_sstring(msg_id) + ": " + str);
    }

    void operator()(const client_info& info, const sstring& str) const {
        log(to_sstring("client ") + inet_ntoa(info.addr.as_posix_sockaddr_in().sin_addr) + ": " + str);
    }

    void operator()(ipv4_addr addr, const sstring& str) const {
        log(to_sstring("client ") + inet_ntoa(in_addr{net::ntoh(addr.ip)}) + ": " + str);
    }
};

class connection {
protected:
    connected_socket _fd;
    input_stream<char> _read_buf;
    output_stream<char> _write_buf;
    bool _error = false;
    bool _connected = false;
    promise<> _stopped;
    stats _stats;
    const logger& _logger;
    // The owner of the pointer below is an instance of rpc::protocol<typename Serializer> class.
    // The type of the pointer is erased here, but the original type is Serializer
    void* _serializer;
    struct outgoing_entry {
        timer<rpc_clock_type> t;
        snd_buf buf;
        compat::optional<promise<>> p = promise<>();
        cancellable* pcancel = nullptr;
        outgoing_entry(snd_buf b) : buf(std::move(b)) {}
        outgoing_entry(outgoing_entry&& o) : t(std::move(o.t)), buf(std::move(o.buf)), p(std::move(o.p)), pcancel(o.pcancel) {
            o.p = compat::nullopt;
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
    // stream related fields
    bool _is_stream = false;
    connection_id _id = invalid_connection_id;

    std::unordered_map<connection_id, xshard_connection_ptr> _streams;
    queue<rcv_buf> _stream_queue = queue<rcv_buf>(max_queued_stream_buffers);
    semaphore _stream_sem = semaphore(max_stream_buffers_memory);
    bool _sink_closed = false;
    bool _source_closed = false;
    // the future holds if sink is already closed
    // if it is not ready it means the sink is been closed
    future<bool> _sink_closed_future = make_ready_future<bool>(false);

    bool is_stream() {
        return _is_stream;
    }

    snd_buf compress(snd_buf buf);
    future<> send_buffer(snd_buf buf);

    enum class outgoing_queue_type {
        request,
        response,
        stream = response
    };

    template<outgoing_queue_type QueueType> void send_loop();
    future<> stop_send_loop();
    future<compat::optional<rcv_buf>>  read_stream_frame_compressed(input_stream<char>& in);
    bool stream_check_twoway_closed() {
        return _sink_closed && _source_closed;
    }
    future<> stream_close();
    future<> stream_process_incoming(rcv_buf&&);
    future<> handle_stream_frame();

public:
    connection(connected_socket&& fd, const logger& l, void* s, connection_id id = invalid_connection_id) : connection(l, s, id) {
        set_socket(std::move(fd));
    }
    connection(const logger& l, void* s, connection_id id = invalid_connection_id) : _logger(l), _serializer(s), _id(id) {}
    virtual ~connection() {}
    void set_socket(connected_socket&& fd);
    future<> send_negotiation_frame(feature_map features);
    // functions below are public because they are used by external heavily templated functions
    // and I am not smart enough to know how to define them as friends
    future<> send(snd_buf buf, compat::optional<rpc_clock_type::time_point> timeout = {}, cancellable* cancel = nullptr);
    bool error() { return _error; }
    void abort();
    future<> stop();
    future<> stream_receive(circular_buffer<foreign_ptr<std::unique_ptr<rcv_buf>>>& bufs);
    future<> close_sink() {
        _sink_closed = true;
        if (stream_check_twoway_closed()) {
            return stream_close();
        }
        return make_ready_future();
    }
    bool sink_closed() {
        return _sink_closed;
    }
    future<> close_source() {
        _source_closed = true;
        if (stream_check_twoway_closed()) {
            return stream_close();
        }
        return make_ready_future();
    }
    connection_id get_connection_id() const {
        return _id;
    }
    xshard_connection_ptr get_stream(connection_id id) const;
    void register_stream(connection_id id, xshard_connection_ptr c);
    virtual ipv4_addr peer_address() const = 0;

    const logger& get_logger() const {
        return _logger;
    }

    template<typename Serializer>
    Serializer& serializer() {
        return *static_cast<Serializer*>(_serializer);
    }

    template <typename FrameType, typename Info>
    typename FrameType::return_type read_frame(const Info& info, input_stream<char>& in);

    template <typename FrameType, typename Info>
    typename FrameType::return_type read_frame_compressed(const Info& info, std::unique_ptr<compressor>& compressor, input_stream<char>& in);
    friend class client;
};

// send data Out...
template<typename Serializer, typename... Out>
class sink_impl : public sink<Out...>::impl {
public:
    sink_impl(xshard_connection_ptr con) : sink<Out...>::impl(std::move(con)) {}
    future<> operator()(const Out&... args) override;
    future<> close() override;
};

// receive data In...
template<typename Serializer, typename... In>
class source_impl : public source<In...>::impl {
public:
    source_impl(xshard_connection_ptr con) : source<In...>::impl(std::move(con)) {}
    future<compat::optional<std::tuple<In...>>> operator()() override;
};

class client : public rpc::connection, public weakly_referencable<client> {
    socket _socket;
    id_type _message_id = 1;
    struct reply_handler_base {
        timer<rpc_clock_type> t;
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
    compat::optional<shared_promise<>> _client_negotiated = shared_promise<>();
    weak_ptr<client> _parent; // for stream clients

private:
    future<> negotiate_protocol(input_stream<char>& in);
    void negotiate(feature_map server_features);
    future<int64_t, compat::optional<rcv_buf>>
    read_response_frame(input_stream<char>& in);
    future<int64_t, compat::optional<rcv_buf>>
    read_response_frame_compressed(input_stream<char>& in);
    void send_loop() {
        if (is_stream()) {
            rpc::connection::send_loop<rpc::connection::outgoing_queue_type::stream>();
        } else {
            rpc::connection::send_loop<rpc::connection::outgoing_queue_type::request>();
        }
    }
public:
    /**
     * Create client object which will attempt to connect to the remote address.
     *
     * @param addr the remote address identifying this client
     * @param local the local address of this client
     */
    client(const logger& l, void* s, ipv4_addr addr, ipv4_addr local = ipv4_addr());
    client(const logger& l, void* s, client_options options, ipv4_addr addr, ipv4_addr local = ipv4_addr());

     /**
     * Create client object which will attempt to connect to the remote address using the
     * specified seastar::socket.
     *
     * @param addr the remote address identifying this client
     * @param local the local address of this client
     * @param socket the socket object use to connect to the remote address
     */
    client(const logger& l, void* s, socket socket, ipv4_addr addr, ipv4_addr local = ipv4_addr());
    client(const logger& l, void* s, client_options options, socket socket, ipv4_addr addr, ipv4_addr local = ipv4_addr());

    stats get_stats() const;
    stats& get_stats_internal() {
        return _stats;
    }
    auto next_message_id() { return _message_id++; }
    void wait_for_reply(id_type id, std::unique_ptr<reply_handler_base>&& h, compat::optional<rpc_clock_type::time_point> timeout, cancellable* cancel);
    void wait_timed_out(id_type id);
    future<> stop();
    void abort_all_streams();
    void deregister_this_stream();
    ipv4_addr peer_address() const override {
        return _server_addr;
    }
    future<> await_connection() {
        if (!_client_negotiated) {
            return make_ready_future<>();
        } else {
            return _client_negotiated->get_shared_future();
        }
    }
    template<typename Serializer, typename... Out>
    future<sink<Out...>> make_stream_sink(socket socket) {
        return await_connection().then([this, socket = std::move(socket)] () mutable {
            if (!this->get_connection_id()) {
                return make_exception_future<sink<Out...>>(std::runtime_error("Streaming is not supported by the server"));
            }
            client_options o = _options;
            o.stream_parent = this->get_connection_id();
            o.send_timeout_data = false;
            auto c = make_shared<client>(_logger, _serializer, o, std::move(socket), _server_addr);
            c->_parent = this->weak_from_this();
            c->_is_stream = true;
            return c->await_connection().then([c, this] {
                xshard_connection_ptr s = make_lw_shared(make_foreign(static_pointer_cast<rpc::connection>(c)));
                this->register_stream(c->get_connection_id(), s);
                return sink<Out...>(make_shared<sink_impl<Serializer, Out...>>(std::move(s)));
            });
        });
    }
    template<typename Serializer, typename... Out>
    future<sink<Out...>> make_stream_sink() {
        return make_stream_sink<Serializer, Out...>(engine().net().socket());
    }
};

class protocol_base;

class server {
private:
    static thread_local std::unordered_map<streaming_domain_type, server*> _servers;

public:
    class connection : public rpc::connection, public enable_shared_from_this<connection> {
        server& _server;
        client_info _info;
        connection_id _parent_id = invalid_connection_id;
        compat::optional<isolation_config> _isolation_config;
    private:
        future<> negotiate_protocol(input_stream<char>& in);
        future<compat::optional<uint64_t>, uint64_t, int64_t, compat::optional<rcv_buf>>
        read_request_frame_compressed(input_stream<char>& in);
        future<feature_map> negotiate(feature_map requested);
        void send_loop() {
            if (is_stream()) {
                rpc::connection::send_loop<rpc::connection::outgoing_queue_type::stream>();
            } else {
                rpc::connection::send_loop<rpc::connection::outgoing_queue_type::response>();
            }
        }
    public:
        connection(server& s, connected_socket&& fd, socket_address&& addr, const logger& l, void* seralizer, connection_id id);
        future<> process();
        future<> respond(int64_t msg_id, snd_buf&& data, compat::optional<rpc_clock_type::time_point> timeout);
        client_info& info() { return _info; }
        const client_info& info() const { return _info; }
        stats get_stats() const {
            stats res = _stats;
            res.pending = _outgoing_queue.size();
            return res;
        }

        stats& get_stats_internal() {
            return _stats;
        }
        ipv4_addr peer_address() const override {
            return ipv4_addr(_info.addr);
        }
        // Resources will be released when this goes out of scope
        future<resource_permit> wait_for_resources(size_t memory_consumed,  compat::optional<rpc_clock_type::time_point> timeout) {
            if (timeout) {
                return get_units(_server._resources_available, memory_consumed, *timeout);
            } else {
                return get_units(_server._resources_available, memory_consumed);
            }
        }
        size_t estimate_request_size(size_t serialized_size) {
            return rpc::estimate_request_size(_server._limits, serialized_size);
        }
        size_t max_request_size() const {
            return _server._limits.max_memory;
        }
        server& get_server() {
            return _server;
        }
        future<> deregister_this_stream();
    };
private:
    protocol_base* _proto;
    server_socket _ss;
    resource_limits _limits;
    rpc_semaphore _resources_available;
    std::unordered_map<connection_id, shared_ptr<connection>> _conns;
    promise<> _ss_stopped;
    gate _reply_gate;
    server_options _options;
    uint64_t _next_client_id = 1;

public:
    server(protocol_base* proto, ipv4_addr addr, resource_limits memory_limit = resource_limits());
    server(protocol_base* proto, server_options opts, ipv4_addr addr, resource_limits memory_limit = resource_limits());
    server(protocol_base* proto, server_socket, resource_limits memory_limit = resource_limits(), server_options opts = server_options{});
    server(protocol_base* proto, server_options opts, server_socket, resource_limits memory_limit = resource_limits());
    void accept();
    future<> stop();
    template<typename Func>
    void foreach_connection(Func&& f) {
        for (auto c : _conns) {
            f(*c.second);
        }
    }
    gate& reply_gate() {
        return _reply_gate;
    }
    friend connection;
    friend client;
};

using rpc_handler_func = std::function<future<> (shared_ptr<server::connection>, compat::optional<rpc_clock_type::time_point> timeout, int64_t msgid,
                                                 rcv_buf data)>;

struct rpc_handler {
    scheduling_group sg;
    rpc_handler_func func;
};

class protocol_base {
public:
    virtual ~protocol_base() {};
    virtual shared_ptr<server::connection> make_server_connection(rpc::server& server, connected_socket fd, socket_address addr, connection_id id) = 0;
    virtual rpc_handler* get_handler(uint64_t msg_id) = 0;
};

// MsgType is a type that holds type of a message. The type should be hashable
// and serializable. It is preferable to use enum for message types, but
// do not forget to provide hash function for it
template<typename Serializer, typename MsgType = uint32_t>
class protocol : public protocol_base {
public:
    class server : public rpc::server {
    public:
        server(protocol& proto, ipv4_addr addr, resource_limits memory_limit = resource_limits()) :
            rpc::server(&proto, addr, memory_limit) {}
        server(protocol& proto, server_options opts, ipv4_addr addr, resource_limits memory_limit = resource_limits()) :
            rpc::server(&proto, opts, addr, memory_limit) {}
        server(protocol& proto, server_socket socket, resource_limits memory_limit = resource_limits(), server_options opts = server_options{}) :
            rpc::server(&proto, std::move(socket), memory_limit) {}
        server(protocol& proto, server_options opts, server_socket socket, resource_limits memory_limit = resource_limits()) :
            rpc::server(&proto, opts, std::move(socket), memory_limit) {}
    };
    class client : public rpc::client {
    public:
        /*
         * Create client object which will attempt to connect to the remote address.
         *
         * @param addr the remote address identifying this client
         * @param local the local address of this client
         */
        client(protocol& p, ipv4_addr addr, ipv4_addr local = ipv4_addr()) :
            rpc::client(p.get_logger(), &p._serializer, addr, local) {}
        client(protocol& p, client_options options, ipv4_addr addr, ipv4_addr local = ipv4_addr()) :
            rpc::client(p.get_logger(), &p._serializer, options, addr, local) {}

        /**
         * Create client object which will attempt to connect to the remote address using the
         * specified seastar::socket.
         *
         * @param addr the remote address identifying this client
         * @param local the local address of this client
         * @param socket the socket object use to connect to the remote address
         */
        client(protocol& p, socket socket, ipv4_addr addr, ipv4_addr local = ipv4_addr()) :
            rpc::client(p.get_logger(), &p._serializer, std::move(socket), addr, local) {}
        client(protocol& p, client_options options, socket socket, ipv4_addr addr, ipv4_addr local = ipv4_addr()) :
            rpc::client(p.get_logger(), &p._serializer, options, std::move(socket), addr, local) {}
    };

    friend server;
private:
    std::unordered_map<MsgType, rpc_handler> _handlers;
    Serializer _serializer;
    logger _logger;

public:
    protocol(Serializer&& serializer) : _serializer(std::forward<Serializer>(serializer)) {}
    template<typename Func>
    auto make_client(MsgType t);

    // returns a function which type depends on Func
    // if Func == Ret(Args...) then return function is
    // future<Ret>(protocol::client&, Args...)
    template<typename Func>
    auto register_handler(MsgType t, Func&& func);

    // returns a function which type depends on Func
    // if Func == Ret(Args...) then return function is
    // future<Ret>(protocol::client&, Args...)
    template <typename Func>
    auto register_handler(MsgType t, scheduling_group sg, Func&& func);

    void unregister_handler(MsgType t) {
        _handlers.erase(t);
    }

    void set_logger(std::function<void(const sstring&)> logger) {
        _logger.set(std::move(logger));
    }

    const logger& get_logger() const {
        return _logger;
    }

    shared_ptr<rpc::server::connection> make_server_connection(rpc::server& server, connected_socket fd, socket_address addr, connection_id id) override {
        return make_shared<rpc::server::connection>(server, std::move(fd), std::move(addr), _logger, &_serializer, id);
    }

    rpc_handler* get_handler(uint64_t msg_id) override {
        auto it = _handlers.find(MsgType(msg_id));
        if (it != _handlers.end()) {
            return &it->second;
        } else {
            return nullptr;
        }
    }

private:
    template<typename Ret, typename... In>
    auto make_client(signature<Ret(In...)> sig, MsgType t);

    void register_receiver(MsgType t, rpc_handler&& handler) {
        _handlers.emplace(t, std::move(handler));
    }
};
}

}

#include "rpc_impl.hh"
