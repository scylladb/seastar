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
#include <variant>
#include <boost/intrusive/list.hpp>
#include <seastar/core/future.hh>
#include <seastar/core/seastar.hh>
#include <seastar/net/api.hh>
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
#include <seastar/util/backtrace.hh>
#include <seastar/util/log.hh>

namespace bi = boost::intrusive;

namespace seastar {

namespace rpc {

/// \defgroup rpc rpc - remote procedure call framework
///
/// \brief
/// rpc is a framework that can be used to define client-server communication
/// protocols.
/// For a high-level description of the RPC features see
/// [doc/rpc.md](./md_rpc.html),
/// [doc/rpc-streaming.md](./md_rpc-streaming.html) and
/// [doc/rpc-compression.md](./md_rpc-compression.html)
///
/// The entry point for setting up an rpc protocol is
/// seastar::rpc::protocol.

using id_type = int64_t;

using rpc_semaphore = basic_semaphore<semaphore_default_exception_factory, rpc_clock_type>;
using resource_permit = semaphore_units<semaphore_default_exception_factory, rpc_clock_type>;

static constexpr char rpc_magic[] = "SSTARRPC";

/// \addtogroup rpc
/// @{

/// Specifies resource isolation for a connection.
struct isolation_config {
    /// Specifies a scheduling group under which the connection (and all its
    /// verb handlers) will execute.
    scheduling_group sched_group = current_scheduling_group();
};

/// Default isolation configuration - run everything in the default scheduling group.
///
/// In the scheduling_group that the protocol::server was created in.
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
    using syncronous_isolation_function = std::function<isolation_config (sstring isolation_cookie)>;
    using asyncronous_isolation_function = std::function<future<isolation_config> (sstring isolation_cookie)>;
    using isolation_function_alternatives = std::variant<syncronous_isolation_function, asyncronous_isolation_function>;
    isolation_function_alternatives isolate_connection = default_isolate_connection;
};

struct client_options {
    std::optional<net::tcp_keepalive_params> keepalive;
    bool tcp_nodelay = true;
    bool reuseaddr = false;
    compressor::factory* compressor_factory = nullptr;
    bool send_timeout_data = true;
    connection_id stream_parent = invalid_connection_id;
    /// Configures how this connection is isolated from other connection on the same server.
    ///
    /// \see resource_limits::isolate_connection
    sstring isolation_cookie;
    sstring metrics_domain = "default";
    bool send_handler_duration = true;
};

/// @}

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

/// \addtogroup rpc
/// @{

class server;

struct server_options {
    compressor::factory* compressor_factory = nullptr;
    bool tcp_nodelay = true;
    std::optional<streaming_domain_type> streaming_domain;
    server_socket::load_balancing_algorithm load_balancing_algorithm = server_socket::load_balancing_algorithm::default_;
    // optional filter function. If set, will be called with remote
    // (connecting) address.
    // Returning false will refuse the incoming connection.
    // Returning true will allow the mechanism to proceed.
    std::function<bool(const socket_address&)> filter_connection = {};
};

/// @}

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
    HANDLER_DURATION = 5,
};

// internal representation of feature data
using feature_map = std::map<protocol_features, sstring>;

// An rpc signature, in the form signature<Ret (In0, In1, In2)>.
template <typename Function>
struct signature;

class logger {
    std::function<void(const sstring&)> _logger;
    ::seastar::logger* _seastar_logger = nullptr;

    // _seastar_logger will always be used first if it's available
    void log(const sstring& str) const {
        if (_seastar_logger) {
            // default level for log messages is `info`
            _seastar_logger->info("{}", str);
        } else if (_logger) {
            _logger(str);
        }
    }

    // _seastar_logger will always be used first if it's available
    template <typename... Args>
#ifdef SEASTAR_LOGGER_COMPILE_TIME_FMT
    void log(log_level level, fmt::format_string<Args...> fmt, Args&&... args) const {
#else
    void log(log_level level, const char* fmt, Args&&... args) const {
#endif
        if (_seastar_logger) {
            _seastar_logger->log(level, fmt, std::forward<Args>(args)...);
        // If the log level is at least `info`, fall back to legacy logging without explicit level.
        // Ignore less severe levels in order not to spam user's log with messages during transition,
        // i.e. when the user still only defines a level-less logger.
        } else if (_logger && level <= log_level::info) {
            fmt::memory_buffer out;
#ifdef SEASTAR_LOGGER_COMPILE_TIME_FMT
            fmt::format_to(fmt::appender(out), fmt, std::forward<Args>(args)...);
#else
            fmt::format_to(fmt::appender(out), fmt::runtime(fmt), std::forward<Args>(args)...);
#endif
            _logger(sstring{out.data(), out.size()});
        }
    }

public:
    void set(std::function<void(const sstring&)> l) {
        _logger = std::move(l);
    }

    void set(::seastar::logger* logger) {
        _seastar_logger = logger;
    }

    void operator()(const client_info& info, id_type msg_id, const sstring& str) const;
    void operator()(const client_info& info, id_type msg_id, log_level level, std::string_view str) const;

    void operator()(const client_info& info, const sstring& str) const;
    void operator()(const client_info& info, log_level level, std::string_view str) const;

    void operator()(const socket_address& addr, const sstring& str) const;
    void operator()(const socket_address& addr, log_level level, std::string_view str) const;
};

class connection {
protected:
    connected_socket _fd;
    input_stream<char> _read_buf;
    output_stream<char> _write_buf;
    bool _error = false;
    bool _connected = false;
    std::optional<shared_promise<>> _negotiated = shared_promise<>();
    promise<> _stopped;
    stats _stats;
    const logger& _logger;
    // The owner of the pointer below is an instance of rpc::protocol<typename Serializer> class.
    // The type of the pointer is erased here, but the original type is Serializer
    void* _serializer;
    struct outgoing_entry : public bi::list_base_hook<bi::link_mode<bi::auto_unlink>> {
        timer<rpc_clock_type> t;
        snd_buf buf;
        promise<> done;
        cancellable* pcancel = nullptr;
        outgoing_entry(snd_buf b) : buf(std::move(b)) {}

        outgoing_entry(outgoing_entry&&) = delete;
        outgoing_entry(const outgoing_entry&) = delete;

        void uncancellable() {
            t.cancel();
            if (pcancel) {
                pcancel->cancel_send = std::function<void()>();
            }
        }

        ~outgoing_entry() {
            if (pcancel) {
                pcancel->cancel_send = std::function<void()>();
                pcancel->send_back_pointer = nullptr;
            }
        }

        using container_t = bi::list<outgoing_entry, bi::constant_time_size<false>>;
    };
    void withdraw(outgoing_entry::container_t::iterator it, std::exception_ptr ex = nullptr);
    future<> _outgoing_queue_ready = _negotiated->get_shared_future();
    outgoing_entry::container_t _outgoing_queue;
    size_t _outgoing_queue_size = 0;
    std::unique_ptr<compressor> _compressor;
    bool _propagate_timeout = false;
    bool _timeout_negotiated = false;
    bool _handler_duration_negotiated = false;
    // stream related fields
    bool _is_stream = false;
    connection_id _id = invalid_connection_id;

    std::unordered_map<connection_id, xshard_connection_ptr> _streams;
    queue<rcv_buf> _stream_queue = queue<rcv_buf>(max_queued_stream_buffers);
    semaphore _stream_sem = semaphore(max_stream_buffers_memory);
    bool _sink_closed = true;
    bool _source_closed = true;
    // the future holds if sink is already closed
    // if it is not ready it means the sink is been closed
    future<bool> _sink_closed_future = make_ready_future<bool>(false);

    void set_negotiated() noexcept;

    bool is_stream() const noexcept {
        return _is_stream;
    }

    snd_buf compress(snd_buf buf);
    future<> send_buffer(snd_buf buf);
    future<> send(snd_buf buf, std::optional<rpc_clock_type::time_point> timeout = {}, cancellable* cancel = nullptr);
    future<> send_entry(outgoing_entry& d) noexcept;
    future<> stop_send_loop(std::exception_ptr ex);
    future<std::optional<rcv_buf>>  read_stream_frame_compressed(input_stream<char>& in);
    bool stream_check_twoway_closed() const noexcept {
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
    size_t outgoing_queue_length() const noexcept {
        return _outgoing_queue_size;
    }

    void set_socket(connected_socket&& fd);
    future<> send_negotiation_frame(feature_map features);
    bool error() const noexcept { return _error; }
    void abort();
    future<> stop() noexcept;
    future<> stream_receive(circular_buffer<foreign_ptr<std::unique_ptr<rcv_buf>>>& bufs);
    future<> close_sink() {
        _sink_closed = true;
        if (stream_check_twoway_closed()) {
            return stream_close();
        }
        return make_ready_future();
    }
    bool sink_closed() const noexcept {
        return _sink_closed;
    }
    future<> close_source() {
        _source_closed = true;
        if (stream_check_twoway_closed()) {
            return stream_close();
        }
        return make_ready_future();
    }
    connection_id get_connection_id() const noexcept {
        return _id;
    }
    stats& get_stats_internal() noexcept {
        return _stats;
    }
    xshard_connection_ptr get_stream(connection_id id) const;
    void register_stream(connection_id id, xshard_connection_ptr c);
    virtual socket_address peer_address() const = 0;

    const logger& get_logger() const noexcept {
        return _logger;
    }

    template<typename Serializer>
    Serializer& serializer() {
        return *static_cast<Serializer*>(_serializer);
    }

    template <typename FrameType>
    future<typename FrameType::return_type> read_frame(socket_address info, input_stream<char>& in);

    template <typename FrameType>
    future<typename FrameType::return_type> read_frame_compressed(socket_address info, std::unique_ptr<compressor>& compressor, input_stream<char>& in);
    friend class client;
    template<typename Serializer, typename... Out>
    friend class sink_impl;
    template<typename Serializer, typename... In>
    friend class source_impl;

    void suspend_for_testing(promise<>& p) {
        _outgoing_queue_ready.get();
        auto dummy = std::make_unique<outgoing_entry>(snd_buf());
        _outgoing_queue.push_back(*dummy);
        _outgoing_queue_ready = dummy->done.get_future();
        (void)p.get_future().then([dummy = std::move(dummy)] { dummy->done.set_value(); });
    }
};

struct deferred_snd_buf {
    promise<> pr;
    snd_buf data;
};

// send data Out...
template<typename Serializer, typename... Out>
class sink_impl : public sink<Out...>::impl {
    // Used on the shard *this lives on.
    alignas (cache_line_size) uint64_t _next_seq_num = 1;

    // Used on the shard the _conn lives on.
    struct alignas (cache_line_size) {
        uint64_t last_seq_num = 0;
        std::map<uint64_t, deferred_snd_buf> out_of_order_bufs;
    } _remote_state;
public:
    sink_impl(xshard_connection_ptr con) : sink<Out...>::impl(std::move(con)) { this->_con->get()->_sink_closed = false; }
    future<> operator()(const Out&... args) override;
    future<> close() override;
    future<> flush() override;
    ~sink_impl() override;
};

// receive data In...
template<typename Serializer, typename... In>
class source_impl : public source<In...>::impl {
public:
    source_impl(xshard_connection_ptr con) : source<In...>::impl(std::move(con)) { this->_con->get()->_source_closed = false; }
    future<std::optional<std::tuple<In...>>> operator()() override;
};

class client : public rpc::connection, public weakly_referencable<client> {
    socket _socket;
    id_type _message_id = 1;
    struct reply_handler_base {
        timer<rpc_clock_type> t;
        cancellable* pcancel = nullptr;
        rpc_clock_type::time_point start;
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

    class metrics {
        struct domain;

        using domain_member_hook_t = boost::intrusive::list_member_hook<boost::intrusive::link_mode<boost::intrusive::auto_unlink>>;

        const client& _c;
        domain& _domain;
        domain_member_hook_t _link;

        using domain_list_t = boost::intrusive::list<metrics,
              boost::intrusive::member_hook<metrics, metrics::domain_member_hook_t, &metrics::_link>,
              boost::intrusive::constant_time_size<false>>;

    public:
        metrics(const client&);
        ~metrics();
    };

    void enqueue_zero_frame();
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
    socket_address _server_addr, _local_addr;
    client_options _options;
    weak_ptr<client> _parent; // for stream clients

    metrics _metrics;

private:
    future<> negotiate_protocol(feature_map map);
    void negotiate(feature_map server_features);
    // Returned future is
    // - message id
    // - optional server-side handler duration
    // - message payload
    future<std::tuple<int64_t, std::optional<uint32_t>, std::optional<rcv_buf>>>
    read_response_frame_compressed(input_stream<char>& in);
public:
    /**
     * Create client object which will attempt to connect to the remote address.
     *
     * @param l \ref seastar::logger to use for logging error messages
     * @param s an optional connection serializer
     * @param addr the remote address identifying this client
     * @param local the local address of this client
     */
    client(const logger& l, void* s, const socket_address& addr, const socket_address& local = {});
    client(const logger& l, void* s, client_options options, const socket_address& addr, const socket_address& local = {});

     /**
     * Create client object which will attempt to connect to the remote address using the
     * specified seastar::socket.
     *
     * @param l \ref seastar::logger to use for logging error messages
     * @param s an optional connection serializer
     * @param addr the remote address identifying this client
     * @param local the local address of this client
     * @param socket the socket object use to connect to the remote address
     */
    client(const logger& l, void* s, socket socket, const socket_address& addr, const socket_address& local = {});
    client(const logger& l, void* s, client_options options, socket socket, const socket_address& addr, const socket_address& local = {});

    stats get_stats() const;
    size_t incoming_queue_length() const noexcept {
        return _outstanding.size();
    }

    auto next_message_id() { return _message_id++; }
    void wait_for_reply(id_type id, std::unique_ptr<reply_handler_base>&& h, std::optional<rpc_clock_type::time_point> timeout, cancellable* cancel);
    void wait_timed_out(id_type id);
    future<> stop() noexcept;
    void abort_all_streams();
    void deregister_this_stream();
    socket_address peer_address() const override {
        return _server_addr;
    }
    future<> await_connection() {
        if (!_negotiated) {
            return make_ready_future<>();
        } else {
            return _negotiated->get_shared_future();
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
            o.metrics_domain += "_stream";
            auto c = make_shared<client>(_logger, _serializer, o, std::move(socket), _server_addr, _local_addr);
            c->_parent = this->weak_from_this();
            c->_is_stream = true;
            return c->await_connection().then([c, this] {
                if (_error) {
                    throw closed_error();
                }
                xshard_connection_ptr s = make_lw_shared(make_foreign(static_pointer_cast<rpc::connection>(c)));
                this->register_stream(c->get_connection_id(), s);
                return sink<Out...>(make_shared<sink_impl<Serializer, Out...>>(std::move(s)));
            }).handle_exception([c] (std::exception_ptr eptr) {
                // If await_connection fails we need to stop the client
                // before destroying it.
                return c->stop().then([eptr, c] {
                    return make_exception_future<sink<Out...>>(eptr);
                });
            });
        });
    }
    template<typename Serializer, typename... Out>
    future<sink<Out...>> make_stream_sink() {
        return make_stream_sink<Serializer, Out...>(make_socket());
    }

    future<> request(uint64_t type, int64_t id, snd_buf buf, std::optional<rpc_clock_type::time_point> timeout = {}, cancellable* cancel = nullptr);
};

class protocol_base;

class server {
private:
    static thread_local std::unordered_map<streaming_domain_type, server*> _servers;

public:
    class connection : public rpc::connection, public enable_shared_from_this<connection> {
        client_info _info;
        connection_id _parent_id = invalid_connection_id;
        std::optional<isolation_config> _isolation_config;
    private:
        future<> negotiate_protocol();
        future<std::tuple<std::optional<uint64_t>, uint64_t, int64_t, std::optional<rcv_buf>>>
        read_request_frame_compressed(input_stream<char>& in);
        future<feature_map> negotiate(feature_map requested);
        future<> send_unknown_verb_reply(std::optional<rpc_clock_type::time_point> timeout, int64_t msg_id, uint64_t type);
    public:
        connection(server& s, connected_socket&& fd, socket_address&& addr, const logger& l, void* seralizer, connection_id id);
        future<> process();
        future<> respond(int64_t msg_id, snd_buf&& data, std::optional<rpc_clock_type::time_point> timeout, std::optional<rpc_clock_type::duration> handler_duration);
        client_info& info() { return _info; }
        const client_info& info() const { return _info; }
        stats get_stats() const {
            stats res = _stats;
            res.pending = outgoing_queue_length();
            return res;
        }
        socket_address peer_address() const override {
            return _info.addr;
        }
        // Resources will be released when this goes out of scope
        future<resource_permit> wait_for_resources(size_t memory_consumed,  std::optional<rpc_clock_type::time_point> timeout) {
            if (timeout) {
                return get_units(get_server()._resources_available, memory_consumed, *timeout);
            } else {
                return get_units(get_server()._resources_available, memory_consumed);
            }
        }
        size_t estimate_request_size(size_t serialized_size) {
            return rpc::estimate_request_size(get_server()._limits, serialized_size);
        }
        size_t max_request_size() const {
            return get_server()._limits.max_memory;
        }
        server& get_server() {
            return _info.server;
        }
        const server& get_server() const {
            return _info.server;
        }
        future<> deregister_this_stream();
        future<> abort_all_streams();
    };
private:
    protocol_base& _proto;
    server_socket _ss;
    resource_limits _limits;
    rpc_semaphore _resources_available;
    std::unordered_map<connection_id, shared_ptr<connection>> _conns;
    promise<> _ss_stopped;
    gate _reply_gate;
    server_options _options;
    bool _shutdown = false;
    uint64_t _next_client_id = 1;

public:
    server(protocol_base* proto, const socket_address& addr, resource_limits memory_limit = resource_limits());
    server(protocol_base* proto, server_options opts, const socket_address& addr, resource_limits memory_limit = resource_limits());
    server(protocol_base* proto, server_socket, resource_limits memory_limit = resource_limits(), server_options opts = server_options{});
    server(protocol_base* proto, server_options opts, server_socket, resource_limits memory_limit = resource_limits());
    void accept();
    /**
     * Stops the server.
     *
     * It makes sure that no new rpcs are admitted, no rpc handlers issued on this
     * connection are running any longer and no replies on the previously running
     * handlers will be sent.
     */
    future<> stop();
    /**
     * Shuts down the server.
     *
     * Light version of the stop, that just makes sure the server is not visible
     * by remote clients, i.e. -- no new rpcs are admitted and no replies on the
     * previously running handlers will be sent. Currently running handlers may
     * still run.
     *
     * Caller of shutdown() mush wait for it to resolve before calling stop.
     */
    future<> shutdown();
    template<typename Func>
    void foreach_connection(Func&& f) {
        for (auto c : _conns) {
            f(*c.second);
        }
    }
    /**
     * Abort the given connection, causing it to stop receiving any further messages.
     * It's safe to abort a connection from an RPC handler running on that connection.
     * Does nothing if there is no connection with the given ID on this server.
     *
     * @param id the ID of the connection to abort.
     */
    void abort_connection(connection_id id);
    gate& reply_gate() {
        return _reply_gate;
    }
    friend connection;
    friend client;
};

using rpc_handler_func = std::function<future<> (shared_ptr<server::connection>, std::optional<rpc_clock_type::time_point> timeout, int64_t msgid,
                                                 rcv_buf data, gate::holder guard)>;

struct rpc_handler {
    scheduling_group sg;
    rpc_handler_func func;
    gate use_gate;
};

class protocol_base {
public:
    virtual ~protocol_base() {};
    virtual shared_ptr<server::connection> make_server_connection(rpc::server& server, connected_socket fd, socket_address addr, connection_id id) = 0;
protected:
    friend class server;

    struct handler_with_holder {
        rpc_handler& handler;
        gate::holder holder;
    };
    virtual std::optional<handler_with_holder> get_handler(uint64_t msg_id) = 0;
};

/// \addtogroup rpc
/// @{

/// Defines a protocol for communication between a server and a client.
///
/// A protocol is defined by a `Serializer` and a `MsgType`. The `Serializer` is
/// responsible for serializing and unserializing all types used as arguments and
/// return types used in the protocol. The `Serializer` is expected to define a
/// `read()` and `write()` method for each such type `T` as follows:
///
///     template <typename Output>
///     void write(const serializer&, Output& output, const T& data);
///
///     template <typename Input>
///     T read(const serializer&, Input& input, type<T> type_tag);  // type_tag used to disambiguate
///
/// Where `Input` and `Output` have a `void read(char*, size_t)` and
/// `write(const char*, size_t)` respectively.
/// `MsgType` defines the type to be used as the message id, the id which is
/// used to identify different messages used in the protocol. These are also
/// often referred to as "verbs". The client will use the message id, to
/// specify the remote method (verb) to invoke on the server. The server uses
/// the message id to dispatch the incoming call to the right handler.
/// `MsgType` should be hashable and serializable. It is preferable to use enum
/// for message types, but do not forget to provide hash function for it.
///
/// Use register_handler() on the server to define the available verbs and the
/// code to be executed when they are invoked by clients. Use make_client() on
/// the client to create a matching callable that can be used to invoke the
/// verb on the server and wait for its result. Note that register_handler()
/// also returns a client, that can be used to invoke the registered verb on
/// another node (given that the other node has the same verb). This is useful
/// for symmetric protocols, where two or more nodes all have servers as well as
/// connect to the other nodes as clients.
///
/// Use protocol::server to listen for and accept incoming connections on the
/// server and protocol::client to establish connections to the server.
/// Note that registering the available verbs can be done before/after
/// listening for connections, but best to ensure that by the time incoming
/// requests are to be expected, all the verbs are set-up.
///
/// ## Configuration
///
/// TODO
///
/// ## Isolation
///
/// RPC supports isolating verb handlers from each other. There are two ways to
/// achieve this: per-handler isolation (the old way) and per-connection
/// isolation (the new way). If no isolation is configured, all handlers will be
/// executed in the context of the scheduling_group in which the
/// protocol::server was created.
///
/// Per-handler isolation (the old way) can be configured by using the
/// register_handler() overload which takes a scheduling_group. When invoked,
/// the body of the handler will be executed from the context of the configured
/// scheduling_group.
///
/// Per-connection isolation (the new way) is a more flexible mechanism that
/// requires user application provided logic to determine how connections are
/// isolated. This mechanism has two parts, the server and the client part.
/// The client configures isolation by setting client_options::isolation_cookie.
/// This cookie is an opaque (to the RPC layer) string that is to be interpreted
/// on the server using user application provided logic. The application
/// provides this logic to the server by setting
/// resource_limits::isolate_connection to an appropriate handler function, that
/// interprets the opaque cookie and resolves it to an isolation_config. The
/// scheduling_group in the former will be used not just to execute all verb
/// handlers, but also the connection loop itself, hence providing better
/// isolation.
///
/// There a few gotchas related to mixing the two isolation mechanisms. This can
/// happen when the application is updated and one of the client/server is
/// still using the old/new mechanism. In general per-connection isolation
/// overrides the per-handler one. If both are set up, the former will determine
/// the scheduling_group context for the handlers. If the client is not
/// configured to send an isolation cookie, the server's
/// resource_limits::isolate_connection will not be invoked and the server will
/// fall back to per-handler isolation if configured. If the client is
/// configured to send an isolation cookie but the server doesn't have a
/// resource_limits::isolate_connection configured, it will use
/// default_isolate_connection() to interpret the cookie. Note that this still
/// overrides the per-handler isolation if any is configured. If the server is
/// so old that it doesn't have the per-connection isolation feature at all, it
/// will of course just use the per-handler one, if configured.
///
/// ## Compatibility
///
/// TODO
///
/// \tparam Serializer the serializer for the protocol.
/// \tparam MsgType the type to be used as the message id or verb id.
template<typename Serializer, typename MsgType = uint32_t>
class protocol final : public protocol_base {
public:
    /// Represents the listening port and all accepted connections.
    class server : public rpc::server {
    public:
        server(protocol& proto, const socket_address& addr, resource_limits memory_limit = resource_limits()) :
            rpc::server(&proto, addr, memory_limit) {}
        server(protocol& proto, server_options opts, const socket_address& addr, resource_limits memory_limit = resource_limits()) :
            rpc::server(&proto, opts, addr, memory_limit) {}
        server(protocol& proto, server_socket socket, resource_limits memory_limit = resource_limits(), server_options = server_options{}) :
            rpc::server(&proto, std::move(socket), memory_limit) {}
        server(protocol& proto, server_options opts, server_socket socket, resource_limits memory_limit = resource_limits()) :
            rpc::server(&proto, opts, std::move(socket), memory_limit) {}
    };
    /// Represents a client side connection.
    class client : public rpc::client {
    public:
        /*
         * Create client object which will attempt to connect to the remote address.
         *
         * @param addr the remote address identifying this client
         * @param local the local address of this client
         */
        client(protocol& p, const socket_address& addr, const socket_address& local = {}) :
            rpc::client(p.get_logger(), &p._serializer, addr, local) {}
        client(protocol& p, client_options options, const socket_address& addr, const socket_address& local = {}) :
            rpc::client(p.get_logger(), &p._serializer, options, addr, local) {}

        /**
         * Create client object which will attempt to connect to the remote address using the
         * specified seastar::socket.
         *
         * @param addr the remote address identifying this client
         * @param local the local address of this client
         * @param socket the socket object use to connect to the remote address
         */
        client(protocol& p, socket socket, const socket_address& addr, const socket_address& local = {}) :
            rpc::client(p.get_logger(), &p._serializer, std::move(socket), addr, local) {}
        client(protocol& p, client_options options, socket socket, const socket_address& addr, const socket_address& local = {}) :
            rpc::client(p.get_logger(), &p._serializer, options, std::move(socket), addr, local) {}
    };

    friend server;
private:
    std::unordered_map<MsgType, rpc_handler> _handlers;
    Serializer _serializer;
    logger _logger;

public:
    protocol(Serializer&& serializer) : _serializer(std::forward<Serializer>(serializer)) {}

    /// Creates a callable that can be used to invoke the verb on the remote.
    ///
    /// \tparam Func The signature of the verb. Has to be either the same or
    ///     compatible with the one passed to register_handler on the server.
    /// \param t the verb to invoke on the remote.
    ///
    /// \returns a callable whose signature is derived from Func as follows:
    ///     given `Func == Ret(Args...)` the returned callable has the following
    ///     signature: `future<Ret>(protocol::client&, Args...)`.
    template<typename Func>
    auto make_client(MsgType t);

    /// Register a handler to be called when this verb is invoked.
    ///
    /// \tparam Func the type of the handler for the verb. This determines the
    ///     signature of the verb.
    /// \param t the verb to register the handler for.
    /// \param func the callable to be called when the verb is invoked by the
    ///     remote.
    ///
    /// \returns a client, a callable that can be used to invoke the verb. See
    ///     make_client(). The client can be discarded, in fact this is what
    ///     most callers will do as real clients will live on a remote node, not
    ///     on the one where handlers are registered.
    template<typename Func>
    auto register_handler(MsgType t, Func&& func);

    /// Register a handler to be called when this verb is invoked.
    ///
    /// \tparam Func the type of the handler for the verb. This determines the
    ///     signature of the verb.
    /// \param t the verb to register the handler for.
    /// \param sg the scheduling group that will be used to invoke the handler
    ///     in. This can be used to execute different verbs in different
    ///     scheduling groups. Note that there is a newer mechanism to determine
    ///     the scheduling groups a handler will run it per invocation, see
    ///     isolation_config.
    /// \param func the callable to be called when the verb is invoked by the
    ///     remote.
    ///
    /// \returns a client, a callable that can be used to invoke the verb. See
    ///     make_client(). The client can be discarded, in fact this is what
    ///     most callers will do as real clients will live on a remote node, not
    ///     on the one where handlers are registered.
    template <typename Func>
    auto register_handler(MsgType t, scheduling_group sg, Func&& func);

    /// Unregister the handler for the verb.
    ///
    /// Waits for all currently running handlers, then unregisters the handler.
    /// Future attempts to invoke the verb will fail. This becomes effective
    /// immediately after calling this function.
    ///
    /// \param t the verb to unregister the handler for.
    ///
    /// \returns a future that becomes available once all currently running
    ///     handlers finished.
    future<> unregister_handler(MsgType t);

    /// Set a logger function to be used to log messages.
    ///
    /// \deprecated use the logger overload set_logger(::seastar::logger*)
    /// instead.
    [[deprecated("Use set_logger(::seastar::logger*) instead")]]
    void set_logger(std::function<void(const sstring&)> logger) {
        _logger.set(std::move(logger));
    }

    /// Set a logger to be used to log messages.
    void set_logger(::seastar::logger* logger) {
        _logger.set(logger);
    }

    const logger& get_logger() const {
        return _logger;
    }

    shared_ptr<rpc::server::connection> make_server_connection(rpc::server& server, connected_socket fd, socket_address addr, connection_id id) override {
        return make_shared<rpc::server::connection>(server, std::move(fd), std::move(addr), _logger, &_serializer, id);
    }

    bool has_handler(MsgType msg_id);

    /// Checks if any there are handlers registered.
    /// Debugging helper, should only be used for debugging and not relied on.
    ///
    /// \returns true if there are, false if there are no registered handlers.
    bool has_handlers() const noexcept {
        return !_handlers.empty();
    }

private:
    std::optional<handler_with_holder> get_handler(uint64_t msg_id) override;

    template<typename Ret, typename... In>
    auto make_client(signature<Ret(In...)> sig, MsgType t);

    void register_receiver(MsgType t, rpc_handler&& handler) {
        auto r = _handlers.emplace(t, std::move(handler));
        if (!r.second) {
            throw_with_backtrace<std::runtime_error>("registered handler already exists");
        }
    }
};

/// @}

}

}

#include "rpc_impl.hh"
