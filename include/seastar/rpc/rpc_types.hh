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

#if FMT_VERSION >= 90000
#include <fmt/ostream.h>
#endif
#if FMT_VERSION >= 100000
#include <fmt/std.h>
#endif

#include <seastar/net/api.hh>
#include <stdexcept>
#include <string>
#include <any>
#include <seastar/util/assert.hh>
#include <seastar/util/std-compat.hh>
#include <seastar/util/variant_utils.hh>
#include <seastar/core/timer.hh>
#include <seastar/core/circular_buffer.hh>
#include <seastar/core/simple-stream.hh>
#include <seastar/core/lowres_clock.hh>
#include <boost/functional/hash.hpp>
#include <seastar/core/sharded.hh>

namespace seastar {

namespace rpc {

using rpc_clock_type = lowres_clock;

// used to tag a type for serializers
template<typename T>
using type = std::type_identity<T>;

struct stats {
    using counter_type = uint64_t;
    counter_type replied = 0;
    counter_type pending = 0;
    counter_type exception_received = 0;
    counter_type sent_messages = 0;
    counter_type wait_reply = 0;
    counter_type timeout = 0;
    counter_type delay_samples = 0;
    std::chrono::duration<double> delay_total = std::chrono::duration<double>(0);
};

class connection_id {
    uint64_t _id;

public:
    uint64_t id() const {
        return _id;
    }
    bool operator==(const connection_id& o) const {
        return _id == o._id;
    }
    explicit operator bool() const {
        return shard() != 0xffff;
    }
    size_t shard() const {
        return size_t(_id & 0xffff);
    }
    constexpr static connection_id make_invalid_id(uint64_t _id = 0) {
        return make_id(_id, 0xffff);
    }
    constexpr static connection_id make_id(uint64_t _id, uint16_t shard) {
        return {_id << 16 | shard};
    }
    constexpr connection_id(uint64_t id) : _id(id) {}
};

constexpr connection_id invalid_connection_id = connection_id::make_invalid_id();

std::ostream& operator<<(std::ostream&, const connection_id&);

class server;

struct client_info {
    socket_address addr;
    rpc::server& server;
    connection_id conn_id;
    std::unordered_map<sstring, std::any> user_data;
    template <typename T>
    void attach_auxiliary(const sstring& key, T&& object) {
        user_data.emplace(key, std::any(std::forward<T>(object)));
    }
    template <typename T>
    T& retrieve_auxiliary(const sstring& key) {
        auto it = user_data.find(key);
        SEASTAR_ASSERT(it != user_data.end());
        return std::any_cast<T&>(it->second);
    }
    template <typename T>
    std::add_const_t<T>& retrieve_auxiliary(const sstring& key) const {
        return const_cast<client_info*>(this)->retrieve_auxiliary<std::add_const_t<T>>(key);
    }
    template <typename T>
    T* retrieve_auxiliary_opt(const sstring& key) noexcept {
        auto it = user_data.find(key);
        if (it == user_data.end()) {
            return nullptr;
        }
        return &std::any_cast<T&>(it->second);
    }
    template <typename T>
    const T* retrieve_auxiliary_opt(const sstring& key) const noexcept {
        auto it = user_data.find(key);
        if (it == user_data.end()) {
            return nullptr;
        }
        return &std::any_cast<const T&>(it->second);
    }
};

class error : public std::runtime_error {
public:
    error(const std::string& msg) : std::runtime_error(msg) {}
};

class closed_error : public error {
public:
    closed_error() : error("connection is closed") {}
};

class timeout_error : public error {
public:
    timeout_error() : error("rpc call timed out") {}
};

class unknown_verb_error : public error {
public:
    uint64_t type;
    unknown_verb_error(uint64_t type_) : error("unknown verb"), type(type_) {}
};

class unknown_exception_error : public error {
public:
    unknown_exception_error() : error("unknown exception") {}
};

class rpc_protocol_error : public error {
public:
    rpc_protocol_error() : error("rpc protocol exception") {}
};

class canceled_error : public error {
public:
    canceled_error() : error("rpc call was canceled") {}
};

class stream_closed : public error {
public:
    stream_closed() : error("rpc stream was closed by peer") {}
};

class remote_verb_error : public error {
    using error::error;
};

struct no_wait_type {};

// return this from a callback if client does not want to waiting for a reply
extern no_wait_type no_wait;

/// \addtogroup rpc
/// @{

template <typename T>
class optional : public std::optional<T> {
public:
     using std::optional<T>::optional;
};

class opt_time_point : public std::optional<rpc_clock_type::time_point> {
public:
     using std::optional<rpc_clock_type::time_point>::optional;
     opt_time_point(std::optional<rpc_clock_type::time_point> time_point) {
         static_cast<std::optional<rpc_clock_type::time_point>&>(*this) = time_point;
     }
};

/// @}

struct cancellable {
    std::function<void()> cancel_send;
    std::function<void()> cancel_wait;
    cancellable** send_back_pointer = nullptr;
    cancellable** wait_back_pointer = nullptr;
    cancellable() = default;
    cancellable(cancellable&& x) : cancel_send(std::move(x.cancel_send)), cancel_wait(std::move(x.cancel_wait)), send_back_pointer(x.send_back_pointer), wait_back_pointer(x.wait_back_pointer) {
        if (send_back_pointer) {
            *send_back_pointer = this;
            x.send_back_pointer = nullptr;
        }
        if (wait_back_pointer) {
            *wait_back_pointer = this;
            x.wait_back_pointer = nullptr;
        }
    }
    cancellable& operator=(cancellable&& x) {
        if (&x != this) {
            this->~cancellable();
            new (this) cancellable(std::move(x));
        }
        return *this;
    }
    void cancel() {
        if (cancel_send) {
            cancel_send();
        }
        if (cancel_wait) {
            cancel_wait();
        }
    }
    ~cancellable() {
        cancel();
    }
};

struct rcv_buf {
    uint32_t size = 0;
    std::optional<semaphore_units<>> su;
    std::variant<std::vector<temporary_buffer<char>>, temporary_buffer<char>> bufs;
    using iterator = std::vector<temporary_buffer<char>>::iterator;
    rcv_buf() {}
    explicit rcv_buf(size_t size_) : size(size_) {}
    explicit rcv_buf(temporary_buffer<char> b) : size(b.size()), bufs(std::move(b)) {};
    explicit rcv_buf(std::vector<temporary_buffer<char>> bufs, size_t size)
        : size(size), bufs(std::move(bufs)) {};
};

struct snd_buf {
    // Preferred, but not required, chunk size.
    static constexpr size_t chunk_size = 128*1024;
    uint32_t size = 0;
    std::variant<std::vector<temporary_buffer<char>>, temporary_buffer<char>> bufs;
    using iterator = std::vector<temporary_buffer<char>>::iterator;
    snd_buf() {}
    snd_buf(snd_buf&&) noexcept;
    snd_buf& operator=(snd_buf&&) noexcept;
    explicit snd_buf(size_t size_);
    explicit snd_buf(temporary_buffer<char> b) : size(b.size()), bufs(std::move(b)) {};

    explicit snd_buf(std::vector<temporary_buffer<char>> bufs, size_t size)
        : size(size), bufs(std::move(bufs)) {};

    temporary_buffer<char>& front();
};

static inline memory_input_stream<rcv_buf::iterator> make_deserializer_stream(rcv_buf& input) {
    auto* b = std::get_if<temporary_buffer<char>>(&input.bufs);
    if (b) {
        return memory_input_stream<rcv_buf::iterator>(memory_input_stream<rcv_buf::iterator>::simple(b->begin(), b->size()));
    } else {
        auto& ar = std::get<std::vector<temporary_buffer<char>>>(input.bufs);
        return memory_input_stream<rcv_buf::iterator>(memory_input_stream<rcv_buf::iterator>::fragmented(ar.begin(), input.size));
    }
}

class compressor {
public:
    virtual ~compressor() {}
    // compress data and leave head_space bytes at the beginning of returned buffer
    virtual snd_buf compress(size_t head_space, snd_buf data) = 0;
    // decompress data
    virtual rcv_buf decompress(rcv_buf data) = 0;
    virtual sstring name() const = 0;
    virtual future<> close() noexcept { return make_ready_future<>(); };

    // factory to create compressor for a connection
    class factory {
    public:
        virtual ~factory() {}
        // return feature string that will be sent as part of protocol negotiation
        virtual const sstring& supported() const = 0;
        // negotiate compress algorithm
        // send_empty_frame() requests an empty frame to be sent to the peer compressor on the other side of the connection.
        // By attaching a header to this empty frame, the compressor can communicate somthing to the peer,
        // send_empty_frame() mustn't be called from inside compress() or decompress().
        virtual std::unique_ptr<compressor> negotiate(sstring feature, bool is_server, std::function<future<>()> send_empty_frame) const {
            return negotiate(feature, is_server);
        }
        virtual std::unique_ptr<compressor> negotiate(sstring feature, bool is_server) const = 0;
    };
};

class connection;

using xshard_connection_ptr = lw_shared_ptr<foreign_ptr<shared_ptr<connection>>>;
constexpr size_t max_queued_stream_buffers = 50;
constexpr size_t max_stream_buffers_memory = 100 * 1024;

/// \addtogroup rpc
/// @{

// send data Out...
template<typename... Out>
class sink {
public:
    class impl {
    protected:
        xshard_connection_ptr _con;
        semaphore _sem;
        std::exception_ptr _ex;
        impl(xshard_connection_ptr con) : _con(std::move(con)), _sem(max_stream_buffers_memory) {}
    public:
        virtual ~impl() {};
        virtual future<> operator()(const Out&... args) = 0;
        virtual future<> close() = 0;
        virtual future<> flush() = 0;
        friend sink;
    };

private:
    shared_ptr<impl> _impl;

public:
    sink(shared_ptr<impl> impl) : _impl(std::move(impl)) {}
    future<> operator()(const Out&... args) {
        return _impl->operator()(args...);
    }
    future<> close() {
        return _impl->close();
    }
    // Calling this function makes sure that any data buffered
    // by the stream sink will be flushed to the network.
    // It does not mean the data was received by the corresponding
    // source.
    future<> flush() {
        return _impl->flush();
    }
    connection_id get_id() const;
};

// receive data In...
template<typename... In>
class source {
public:
    class impl {
    protected:
        xshard_connection_ptr _con;
        circular_buffer<foreign_ptr<std::unique_ptr<rcv_buf>>> _bufs;
        impl(xshard_connection_ptr con) : _con(std::move(con)) {
            _bufs.reserve(max_queued_stream_buffers);
        }
    public:
        virtual ~impl() {}
        virtual future<std::optional<std::tuple<In...>>> operator()() = 0;
        friend source;
    };
private:
    shared_ptr<impl> _impl;

public:
    source(shared_ptr<impl> impl) : _impl(std::move(impl)) {}
    future<std::optional<std::tuple<In...>>> operator()() {
        return _impl->operator()();
    };
    connection_id get_id() const;
    template<typename Serializer, typename... Out> sink<Out...> make_sink();
};

/// Used to return multiple values in rpc without variadic futures
///
/// If you wish to return multiple values from an rpc procedure, use a
/// signature `future<rpc::tuple<return type list> (argument list)>>`. This
/// will be marshalled by rpc, so you do not need to have your Serializer
/// serialize/deserialize this tuple type. The serialization format is
/// compatible with the deprecated variadic future support, and is compatible
/// with adding new return types in a backwards compatible way provided new
/// parameters are appended only, and wrapped with rpc::optional:
/// `future<rpc::tuple<existing return type list, rpc::optional<new_return_type>>> (argument list)`
///
/// You may also use another tuple type, such as std::tuple. In this case,
/// your Serializer type must recognize your tuple type and provide serialization
/// and deserialization for it.
template <typename... T>
class tuple : public std::tuple<T...> {
public:
    using std::tuple<T...>::tuple;
    tuple(std::tuple<T...>&& x) : std::tuple<T...>(std::move(x)) {}
};

/// @}

#ifndef SEASTAR_P2581R1
template <typename... T>
tuple(T&&...) ->  tuple<T...>;
#endif

} // namespace rpc

}

namespace std {
template<>
struct hash<seastar::rpc::connection_id> {
    size_t operator()(const seastar::rpc::connection_id& id) const {
        size_t h = 0;
        boost::hash_combine(h, std::hash<uint64_t>{}(id.id()));
        return h;
    }
};

template <typename... T>
struct tuple_size<seastar::rpc::tuple<T...>> : tuple_size<tuple<T...>> {
};

template <size_t I, typename... T>
struct tuple_element<I, seastar::rpc::tuple<T...>> : tuple_element<I, tuple<T...>> {
};

}

#if FMT_VERSION >= 90000
template <> struct fmt::formatter<seastar::rpc::connection_id> : fmt::ostream_formatter {};
#endif

#if FMT_VERSION < 100000
// fmt v10 introduced formatter for std::exception
template <std::derived_from<seastar::rpc::error> T>
struct fmt::formatter<T> : fmt::formatter<string_view> {
    auto format(const T& e, fmt::format_context& ctx) const {
        return fmt::format_to(ctx.out(), "{}", e.what());
    }
};
#endif

#if FMT_VERSION < 100000
template <typename T>
struct fmt::formatter<seastar::rpc::optional<T>> {
    constexpr auto parse(format_parse_context& ctx) { return ctx.begin(); }
    auto format(const seastar::rpc::optional<T>& opt, fmt::format_context& ctx) const {
        if (opt) {
            return fmt::format_to(ctx.out(), "optional({})", *opt);
        } else {
            return fmt::format_to(ctx.out(), "none");
        }
    }
};
#else
template <typename T>
struct fmt::formatter<seastar::rpc::optional<T>> : private fmt::formatter<std::optional<T>> {
    using fmt::formatter<std::optional<T>>::parse;
    auto format(const seastar::rpc::optional<T>& opt, fmt::format_context& ctx) const {
        return fmt::formatter<std::optional<T>>::format(opt, ctx);
    }
};
#endif
