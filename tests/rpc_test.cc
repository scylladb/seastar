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


#include "loopback_socket.hh"
#include "rpc/rpc.hh"
#include "rpc/lz4_compressor.hh"
#include "rpc/multi_algo_compressor_factory.hh"
#include "test-utils.hh"
#include "core/thread.hh"
#include "core/sleep.hh"

using namespace seastar;

struct serializer {
};

template <typename T, typename Output>
inline
void write_arithmetic_type(Output& out, T v) {
    static_assert(std::is_arithmetic<T>::value, "must be arithmetic type");
    return out.write(reinterpret_cast<const char*>(&v), sizeof(T));
}

template <typename T, typename Input>
inline
T read_arithmetic_type(Input& in) {
    static_assert(std::is_arithmetic<T>::value, "must be arithmetic type");
    T v;
    in.read(reinterpret_cast<char*>(&v), sizeof(T));
    return v;
}

template <typename Output>
inline void write(serializer, Output& output, int32_t v) { return write_arithmetic_type(output, v); }
template <typename Output>
inline void write(serializer, Output& output, uint32_t v) { return write_arithmetic_type(output, v); }
template <typename Output>
inline void write(serializer, Output& output, int64_t v) { return write_arithmetic_type(output, v); }
template <typename Output>
inline void write(serializer, Output& output, uint64_t v) { return write_arithmetic_type(output, v); }
template <typename Output>
inline void write(serializer, Output& output, double v) { return write_arithmetic_type(output, v); }
template <typename Input>
inline int32_t read(serializer, Input& input, rpc::type<int32_t>) { return read_arithmetic_type<int32_t>(input); }
template <typename Input>
inline uint32_t read(serializer, Input& input, rpc::type<uint32_t>) { return read_arithmetic_type<uint32_t>(input); }
template <typename Input>
inline uint64_t read(serializer, Input& input, rpc::type<uint64_t>) { return read_arithmetic_type<uint64_t>(input); }
template <typename Input>
inline uint64_t read(serializer, Input& input, rpc::type<int64_t>) { return read_arithmetic_type<int64_t>(input); }
template <typename Input>
inline double read(serializer, Input& input, rpc::type<double>) { return read_arithmetic_type<double>(input); }

template <typename Output>
inline void write(serializer, Output& out, const sstring& v) {
    write_arithmetic_type(out, uint32_t(v.size()));
    out.write(v.c_str(), v.size());
}

template <typename Input>
inline sstring read(serializer, Input& in, rpc::type<sstring>) {
    auto size = read_arithmetic_type<uint32_t>(in);
    sstring ret(sstring::initialized_later(), size);
    in.read(ret.begin(), size);
    return ret;
}

using test_rpc_proto = rpc::protocol<serializer>;
using connect_fn = std::function<test_rpc_proto::client (ipv4_addr addr)>;

class rpc_socket_impl : public net::socket_impl {
    promise<connected_socket> _p;
    bool _connect;
    loopback_socket_impl _socket;
public:
    rpc_socket_impl(loopback_connection_factory& factory, bool connect)
            : _connect(connect), _socket(factory) {
    }
    virtual future<connected_socket> connect(socket_address sa, socket_address local, transport proto = transport::TCP) override {
        return _connect ? _socket.connect(sa, local, proto) : _p.get_future();
    }
    virtual void shutdown() override {
        if (_connect) {
            _socket.shutdown();
        } else {
            _p.set_exception(std::make_exception_ptr(std::system_error(ECONNABORTED, std::system_category())));
        }
    }
};

future<>
with_rpc_env(rpc::resource_limits resource_limits, rpc::client_options co, rpc::server_options so, bool connect,
        std::function<future<> (test_rpc_proto& proto, test_rpc_proto::server& server, connect_fn connect)> test_fn) {
    struct state {
        test_rpc_proto proto{serializer()};
        loopback_connection_factory lcf;
        std::unique_ptr<test_rpc_proto::server> server;
    };
    return do_with(state(), [=] (state& s) {
        s.server = std::make_unique<test_rpc_proto::server>(s.proto, so, s.lcf.get_server_socket(), resource_limits);
        auto make_client = [&s, connect, co] (ipv4_addr addr) {
            auto socket = seastar::socket(std::make_unique<rpc_socket_impl>(s.lcf, connect));
            return test_rpc_proto::client(s.proto, co, std::move(socket), addr);
        };
        return test_fn(s.proto, *s.server, make_client).finally([&] {
            return s.server->stop();
        });
    });
}

struct cfactory : rpc::compressor::factory {
    mutable int use_compression = 0;
    const sstring name;
    cfactory(sstring name_ = "LZ4") : name(std::move(name_)) {}
    const sstring& supported() const override {
        return name;
    }
    std::unique_ptr<rpc::compressor> negotiate(sstring feature, bool is_server) const override {
        if (feature == name) {
            use_compression++;
            return std::make_unique<rpc::lz4_compressor>();
        } else {
            return nullptr;
        }
    }
};

SEASTAR_TEST_CASE(test_rpc_connect) {
    std::vector<future<>> fs;

    for (auto i = 0; i < 2; i++) {
        for (auto j = 0; j < 4; j++) {
            auto factory = std::make_unique<cfactory>();
            rpc::server_options so;
            rpc::client_options co;
            if (i == 1) {
                so.compressor_factory = factory.get();
            }
            if (j & 1) {
                co.compressor_factory = factory.get();
            }
            co.send_timeout_data = j & 2;
            auto f = with_rpc_env({}, co, so, true, [] (test_rpc_proto& proto, test_rpc_proto::server& s, connect_fn connect) {
                return seastar::async([&proto, &s, connect] {
                    auto c1 = connect(ipv4_addr());
                    auto sum = proto.register_handler(1, [](int a, int b) {
                        return make_ready_future<int>(a+b);
                    });
                    auto result = sum(c1, 2, 3).get0();
                    BOOST_REQUIRE_EQUAL(result, 2 + 3);
                    c1.stop().get();
                });
            }).handle_exception([] (auto ep) {
                BOOST_FAIL("No exception expected");
            }).finally([factory = std::move(factory), i, j = j & 1] {
                if (i == 1 && j == 1) {
                    BOOST_REQUIRE_EQUAL(factory->use_compression, 2);
                } else {
                    BOOST_REQUIRE_EQUAL(factory->use_compression, 0);
                }
            });
            fs.emplace_back(std::move(f));
        }
    }
    return when_all(fs.begin(), fs.end()).discard_result();
}

SEASTAR_TEST_CASE(test_rpc_connect_multi_compression_algo) {
    auto factory1 = std::make_unique<cfactory>();
    auto factory2 = std::make_unique<cfactory>("LZ4NEW");
    rpc::server_options so;
    rpc::client_options co;
    static rpc::multi_algo_compressor_factory server({factory1.get(), factory2.get()});
    static rpc::multi_algo_compressor_factory client({factory2.get(), factory1.get()});
    so.compressor_factory = &server;
    co.compressor_factory = &client;
    return with_rpc_env({}, co, so, true, [] (test_rpc_proto& proto, test_rpc_proto::server& s, connect_fn connect) {
        return seastar::async([&proto, &s, connect] {
            auto c1 = connect(ipv4_addr());
            auto sum = proto.register_handler(1, [](int a, int b) {
                return make_ready_future<int>(a+b);
            });
            auto result = sum(c1, 2, 3).get0();
            BOOST_REQUIRE_EQUAL(result, 2 + 3);
            c1.stop().get();
        });
    }).finally([factory1 = std::move(factory1), factory2 = std::move(factory2)] {
        BOOST_REQUIRE_EQUAL(factory1->use_compression, 0);
        BOOST_REQUIRE_EQUAL(factory2->use_compression, 2);
    });
}

SEASTAR_TEST_CASE(test_rpc_connect_abort) {
    return with_rpc_env({}, {}, {}, false, [] (test_rpc_proto& proto, test_rpc_proto::server& s, connect_fn connect) {
        return seastar::async([&proto, &s, connect] {
            auto c1 = connect(ipv4_addr());
            auto f = proto.register_handler(1, []() { return make_ready_future<>(); });
            c1.stop().get0();
            try {
                f(c1).get0();
                BOOST_REQUIRE(false);
            } catch (...) {}
        });
    });
}

SEASTAR_TEST_CASE(test_rpc_cancel) {
    using namespace std::chrono_literals;
    return with_rpc_env({}, {}, {}, true, [] (test_rpc_proto& proto, test_rpc_proto::server& s, connect_fn connect) {
        return seastar::async([&proto, &s, connect] {
            auto c1 = connect(ipv4_addr());
            bool rpc_executed = false;
            int good = 0;
            promise<> handler_called;
            future<> f_handler_called = handler_called.get_future();
            auto call = proto.register_handler(1, [&rpc_executed,  handler_called = std::move(handler_called)] () mutable {
                handler_called.set_value(); rpc_executed = true; return sleep(1ms);
            });
            rpc::cancellable cancel;
            auto f = call(c1, cancel);
            // cancel send side
            cancel.cancel();
            try {
                f.get();
            } catch(rpc::canceled_error&) {
                good += !rpc_executed;
            };
            f = call(c1, cancel);
            // cancel wait side
            f_handler_called.then([cancel = std::move(cancel)] () mutable {
                cancel.cancel();
            }).get();
            try {
                f.get();
            } catch(rpc::canceled_error&) {
                good += 10*rpc_executed;
            };
            c1.stop().get();
            BOOST_REQUIRE_EQUAL(good, 11);
        });
    });
}
