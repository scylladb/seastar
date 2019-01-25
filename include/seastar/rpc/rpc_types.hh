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

#include <seastar/net/api.hh>
#include <stdexcept>
#include <string>
#include <boost/any.hpp>
#include <boost/type.hpp>
#include <seastar/util/std-compat.hh>
#include <boost/variant.hpp>
#include <seastar/core/timer.hh>
#include <seastar/core/simple-stream.hh>
#include <seastar/core/lowres_clock.hh>
#include <boost/functional/hash.hpp>
#include <seastar/core/sharded.hh>

namespace seastar {

namespace rpc {

using rpc_clock_type = lowres_clock;

// used to tag a type for serializers
template<typename T>
using type = boost::type<T>;

struct stats {
    using counter_type = uint64_t;
    counter_type replied = 0;
    counter_type pending = 0;
    counter_type exception_received = 0;
    counter_type sent_messages = 0;
    counter_type wait_reply = 0;
    counter_type timeout = 0;
};


struct client_info {
    socket_address addr;
    std::unordered_map<sstring, boost::any> user_data;
    template <typename T>
    void attach_auxiliary(const sstring& key, T&& object) {
        user_data.emplace(key, boost::any(std::forward<T>(object)));
    }
    template <typename T>
    T& retrieve_auxiliary(const sstring& key) {
        auto it = user_data.find(key);
        assert(it != user_data.end());
        return boost::any_cast<T&>(it->second);
    }
    template <typename T>
    typename std::add_const<T>::type& retrieve_auxiliary(const sstring& key) const {
        return const_cast<client_info*>(this)->retrieve_auxiliary<typename std::add_const<T>::type>(key);
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

struct no_wait_type {};

// return this from a callback if client does not want to waiting for a reply
extern no_wait_type no_wait;

template <typename T>
class optional : public compat::optional<T> {
public:
     using compat::optional<T>::optional;
};

class opt_time_point : public compat::optional<rpc_clock_type::time_point> {
public:
     using compat::optional<rpc_clock_type::time_point>::optional;
     opt_time_point(compat::optional<rpc_clock_type::time_point> time_point) {
         static_cast<compat::optional<rpc_clock_type::time_point>&>(*this) = time_point;
     }
};

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
    compat::optional<semaphore_units<>> su;
    compat::variant<std::vector<temporary_buffer<char>>, temporary_buffer<char>> bufs;
    using iterator = std::vector<temporary_buffer<char>>::iterator;
    rcv_buf() {}
    explicit rcv_buf(size_t size_) : size(size_) {}
};

struct snd_buf {
    static constexpr size_t chunk_size = 128*1024;
    uint32_t size = 0;
    compat::variant<std::vector<temporary_buffer<char>>, temporary_buffer<char>> bufs;
    using iterator = std::vector<temporary_buffer<char>>::iterator;
    snd_buf() {}
    explicit snd_buf(size_t size_);
    explicit snd_buf(temporary_buffer<char> b) : size(b.size()), bufs(std::move(b)) {};
    temporary_buffer<char>& front();
};

static inline memory_input_stream<rcv_buf::iterator> make_deserializer_stream(rcv_buf& input) {
    auto* b = compat::get_if<temporary_buffer<char>>(&input.bufs);
    if (b) {
        return memory_input_stream<rcv_buf::iterator>(memory_input_stream<rcv_buf::iterator>::simple(b->begin(), b->size()));
    } else {
        auto& ar = compat::get<std::vector<temporary_buffer<char>>>(input.bufs);
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

    // factory to create compressor for a connection
    class factory {
    public:
        virtual ~factory() {}
        // return feature string that will be sent as part of protocol negotiation
        virtual const sstring& supported() const = 0;
        // negotiate compress algorithm
        virtual std::unique_ptr<compressor> negotiate(sstring feature, bool is_server) const = 0;
    };
};

class connection;

struct connection_id {
    uint64_t id;
    bool operator==(const connection_id& o) const {
        return id == o.id;
    }
    operator bool() const {
        return id;
    }
    size_t shard() {
        return size_t(id & 0xffff);
    }
};

constexpr connection_id invalid_connection_id{0};

std::ostream& operator<<(std::ostream&, const connection_id&);

using xshard_connection_ptr = lw_shared_ptr<foreign_ptr<shared_ptr<connection>>>;
constexpr size_t max_queued_stream_buffers = 50;
constexpr size_t max_stream_buffers_memory = 100 * 1024;

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
        virtual future<compat::optional<std::tuple<In...>>> operator()() = 0;
        friend source;
    };
private:
    shared_ptr<impl> _impl;

public:
    source(shared_ptr<impl> impl) : _impl(std::move(impl)) {}
    future<compat::optional<std::tuple<In...>>> operator()() {
        return _impl->operator()();
    };
    connection_id get_id() const;
    template<typename Serializer, typename... Out> sink<Out...> make_sink();
};

} // namespace rpc

}

namespace std {
template<>
struct hash<seastar::rpc::connection_id> {
    size_t operator()(const seastar::rpc::connection_id& id) const {
        size_t h = 0;
        boost::hash_combine(h, std::hash<uint64_t>{}(id.id));
        return h;
    }
};
}
