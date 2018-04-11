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
};

struct client_options {
    std::experimental::optional<net::tcp_keepalive_params> keepalive;
    bool tcp_nodelay = true;
    compressor::factory* compressor_factory = nullptr;
    bool send_timeout_data = true;
};

struct server_options {
    compressor::factory* compressor_factory = nullptr;
    bool tcp_nodelay = true;
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

    snd_buf compress(snd_buf buf);
    future<> send_buffer(snd_buf buf);

    enum class outgoing_queue_type {
        request,
        response
    };

    template<outgoing_queue_type QueueType> void send_loop();
    void send_loop(outgoing_queue_type type);
    future<> stop_send_loop();

public:
    connection(connected_socket&& fd, const logger& l, void* s) : _fd(std::move(fd)), _read_buf(_fd.input()), _write_buf(_fd.output()), _connected(true), _logger(l), _serializer(s) {}
    connection(const logger& l, void* s) : _logger(l), _serializer(s) {}
    void set_socket(connected_socket&& fd);
    future<> send_negotiation_frame(feature_map features);
    // functions below are public because they are used by external heavily templated functions
    // and I am not smart enough to know how to define them as friends
    future<> send(snd_buf buf, std::experimental::optional<rpc_clock_type::time_point> timeout = {}, cancellable* cancel = nullptr);
    bool error() { return _error; }
    future<> stop();
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
};

class client : public rpc::connection {
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
private:
    future<> negotiate_protocol(input_stream<char>& in);
    void negotiate(feature_map server_features);
    future<int64_t, std::experimental::optional<rcv_buf>>
    read_response_frame(input_stream<char>& in);
    future<int64_t, std::experimental::optional<rcv_buf>>
    read_response_frame_compressed(input_stream<char>& in);
    void send_loop() {
        rpc::connection::send_loop(rpc::connection::outgoing_queue_type::request);
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
    void wait_for_reply(id_type id, std::unique_ptr<reply_handler_base>&& h, std::experimental::optional<rpc_clock_type::time_point> timeout, cancellable* cancel);
    void wait_timed_out(id_type id);
    future<> stop();
    ipv4_addr peer_address() const {
        return _server_addr;
    }
};

class protocol_base;

class server {
public:
    class connection : public rpc::connection, public enable_lw_shared_from_this<connection> {
        server& _server;
        client_info _info;
    private:
        future<> negotiate_protocol(input_stream<char>& in);
        future<std::experimental::optional<uint64_t>, uint64_t, int64_t, std::experimental::optional<rcv_buf>>
        read_request_frame_compressed(input_stream<char>& in);
        feature_map negotiate(feature_map requested);
        void send_loop() {
            rpc::connection::send_loop(rpc::connection::outgoing_queue_type::response);
        }
    public:
        connection(server& s, connected_socket&& fd, socket_address&& addr, const logger& l, void* seralizer);
        future<> process();
        future<> respond(int64_t msg_id, snd_buf&& data, std::experimental::optional<rpc_clock_type::time_point> timeout);
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
        ipv4_addr peer_address() const {
            return ipv4_addr(_info.addr);
        }
        // Resources will be released when this goes out of scope
        future<resource_permit> wait_for_resources(size_t memory_consumed,  std::experimental::optional<rpc_clock_type::time_point> timeout) {
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
    };
private:
    protocol_base* _proto;
    server_socket _ss;
    resource_limits _limits;
    rpc_semaphore _resources_available;
    std::unordered_set<lw_shared_ptr<connection>> _conns;
    promise<> _ss_stopped;
    gate _reply_gate;
    server_options _options;
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
            f(*c);
        }
    }
    gate& reply_gate() {
        return _reply_gate;
    }
    friend connection;
};

using rpc_handler = std::function<future<> (lw_shared_ptr<server::connection>, std::experimental::optional<rpc_clock_type::time_point> timeout, int64_t msgid,
                                            rcv_buf data)>;

class protocol_base {
public:
    virtual ~protocol_base() {};
    virtual lw_shared_ptr<server::connection> make_server_connection(rpc::server& server, connected_socket fd, socket_address addr) = 0;
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

    void unregister_handler(MsgType t) {
        _handlers.erase(t);
    }

    void set_logger(std::function<void(const sstring&)> logger) {
        _logger.set(std::move(logger));
    }

    const logger& get_logger() const {
        return _logger;
    }

    lw_shared_ptr<rpc::server::connection> make_server_connection(rpc::server& server, connected_socket fd, socket_address addr) override {
        return make_lw_shared<rpc::server::connection>(server, std::move(fd), std::move(addr), _logger, &_serializer);
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
