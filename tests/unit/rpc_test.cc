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
#include <seastar/rpc/rpc.hh>
#include <seastar/rpc/rpc_types.hh>
#include <seastar/rpc/lz4_compressor.hh>
#include <seastar/rpc/multi_algo_compressor_factory.hh>
#include <seastar/testing/test_case.hh>
#include <seastar/testing/thread_test_case.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/sleep.hh>
#include <seastar/util/defer.hh>

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
using make_socket_fn = std::function<seastar::socket ()>;

struct rpc_loopback_error_injector : public loopback_error_injector {
    int _x = 0;
    bool server_rcv_error() override {
        return _x++ >= 50;
    }
};

class rpc_socket_impl : public ::net::socket_impl {
    promise<connected_socket> _p;
    bool _connect;
    loopback_socket_impl _socket;
    rpc_loopback_error_injector _error_injector;
public:
    rpc_socket_impl(loopback_connection_factory& factory, bool connect, bool inject_error)
            : _connect(connect),
              _socket(factory, inject_error ? &_error_injector : nullptr) {
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
with_rpc_env(rpc::resource_limits resource_limits, rpc::server_options so, bool connect, bool inject_error,
        std::function<future<> (test_rpc_proto& proto, test_rpc_proto::server& server, make_socket_fn make_socket)> test_fn) {
    struct state {
        test_rpc_proto proto{serializer()};
        loopback_connection_factory lcf;
        std::vector<std::unique_ptr<test_rpc_proto::server>> servers;
    };
    return do_with(state(), [=] (state& s) {
        s.servers.resize(smp::count);
        return smp::invoke_on_all([=, &s] {
            s.servers[engine().cpu_id()] =  std::make_unique<test_rpc_proto::server>(s.proto, so, s.lcf.get_server_socket(), resource_limits);
        }).then([=, &s] {
            auto make_socket = [&s, connect, inject_error] () {
                return seastar::socket(std::make_unique<rpc_socket_impl>(s.lcf, connect, inject_error));
            };
            return test_fn(s.proto, *s.servers[0], make_socket).finally([&] {
                return smp::invoke_on_all([&s] {
                    auto sptr = s.servers[engine().cpu_id()].get();
                    s.lcf.destroy_shard(engine().cpu_id());
                    return sptr->stop().finally([p = std::move(s.servers[engine().cpu_id()])] {});
                });
            });
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
#if 1
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
            auto f = with_rpc_env({}, so, true, false, [co] (test_rpc_proto& proto, test_rpc_proto::server& s, make_socket_fn make_socket) {
                return seastar::async([&proto, make_socket, co] {
                    test_rpc_proto::client c1(proto, co, make_socket(), ipv4_addr());
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
    return with_rpc_env({}, so, true, false, [co] (test_rpc_proto& proto, test_rpc_proto::server& s, make_socket_fn make_socket) {
        return seastar::async([&proto, make_socket, co] {
            test_rpc_proto::client c1(proto, co, make_socket(), ipv4_addr());
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
    return with_rpc_env({}, {}, false, false, [] (test_rpc_proto& proto, test_rpc_proto::server& s, make_socket_fn make_socket) {
        return seastar::async([&proto, make_socket] {
            test_rpc_proto::client c1(proto, {}, make_socket(), ipv4_addr());
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
    return with_rpc_env({}, {}, true, false, [] (test_rpc_proto& proto, test_rpc_proto::server& s, make_socket_fn make_socket) {
        return seastar::async([&proto, make_socket] {
            test_rpc_proto::client c1(proto, {}, make_socket(), ipv4_addr());
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
            f_handler_called.then([&cancel] {
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

SEASTAR_TEST_CASE(test_message_to_big) {
    return with_rpc_env({0, 1, 100}, {}, true, false, [] (test_rpc_proto& proto, test_rpc_proto::server& s, make_socket_fn make_socket) {
        return seastar::async([&proto, make_socket] {
            test_rpc_proto::client c(proto, {}, make_socket(), ipv4_addr());
            bool good = true;
            auto call = proto.register_handler(1, [&] (sstring payload) mutable {
                good = false;
            });
            try {
                call(c, sstring(sstring::initialized_later(), 101)).get();
                good = false;
            } catch(std::runtime_error& err) {
            } catch(...) {
                good = false;
            }
            c.stop().get();
            BOOST_REQUIRE_EQUAL(good, true);
        });
    });
}
#endif

struct stream_test_result {
    bool client_source_closed = false;
    bool server_source_closed = false;
    bool sink_exception = false;
    bool sink_close_exception = false;
    bool source_done_exception = false;
    bool server_done_exception = false;
    bool client_stop_exception = false;
    int server_sum = 0;
};

future<stream_test_result> stream_test_func(test_rpc_proto& proto, make_socket_fn make_socket, bool stop_client) {
    return seastar::async([&proto, make_socket, stop_client] {
        stream_test_result r;
        test_rpc_proto::client c(proto, {}, make_socket(), ipv4_addr());
        future<> server_done = make_ready_future();
        proto.register_handler(1, [&](int i, rpc::source<int> source) {
            BOOST_REQUIRE_EQUAL(i, 666);
            auto sink = source.make_sink<serializer, sstring>();
            auto sink_loop = seastar::async([sink] () mutable {
                for (auto i = 0; i < 100; i++) {
                    sink("seastar").get();
                    sleep(std::chrono::milliseconds(1)).get();
                }
                sink.close().get();
            });
            auto source_loop = seastar::async([source, &r] () mutable {
                while (!r.server_source_closed) {
                    auto data = source().get0();
                    if (data) {
                        r.server_sum += std::get<0>(*data);
                    } else {
                        r.server_source_closed = true;
                    }
                }
            });
            server_done = when_all_succeed(std::move(sink_loop), std::move(source_loop)).discard_result();
            return sink;
        });
        auto call = proto.make_client<rpc::source<sstring> (int, rpc::sink<int>)>(1);
        auto x = [&] {
            try {
                return c.make_stream_sink<serializer, int>(make_socket()).get0();
            } catch (...) {
                c.stop().get();
                throw;
            }
        };
        auto sink = x();
        auto source = call(c, 666, sink).get0();
        auto source_done = seastar::async([&] {
            while (!r.client_source_closed) {
                auto data = source().get0();
                if (data) {
                    BOOST_REQUIRE_EQUAL(std::get<0>(*data), "seastar");
                } else {
                    r.client_source_closed = true;
                }
            }
        });
        auto check_exception = [] (auto f) {
            try {
                f.get();
            } catch (...) {
                return true;
            }
            return false;
        };
        future<> stop_client_future = make_ready_future();
        for (int i = 1; i < 101; i++) {
            if (stop_client && i == 50) {
                // stop client while stream is in use
                stop_client_future = c.stop();
            }
            sleep(std::chrono::milliseconds(1)).get();
            r.sink_exception = check_exception(sink(i));
            if (r.sink_exception) {
                break;
            }
        }
        r.sink_close_exception = check_exception(sink.close());
        r.source_done_exception = check_exception(std::move(source_done));
        r.server_done_exception = check_exception(std::move(server_done));
        r.client_stop_exception = check_exception(!stop_client ? c.stop() : std::move(stop_client_future));
        return r;
    });
}

SEASTAR_TEST_CASE(test_stream_simple) {
    rpc::server_options so;
    so.streaming_domain = rpc::streaming_domain_type(1);
    return with_rpc_env({}, so, true, false, [] (test_rpc_proto& proto, test_rpc_proto::server& s, make_socket_fn make_socket) {
        return stream_test_func(proto, make_socket, false).then([] (stream_test_result r) {
            BOOST_REQUIRE(r.client_source_closed &&
                    r.server_source_closed &&
                    r.server_sum == 5050 &&
                    !r.sink_exception &&
                    !r.sink_close_exception &&
                    !r.source_done_exception &&
                    !r.server_done_exception &&
                    !r.client_stop_exception);
        });
    });
}

SEASTAR_TEST_CASE(test_stream_stop_client) {
    rpc::server_options so;
    so.streaming_domain = rpc::streaming_domain_type(1);
    return with_rpc_env({}, so, true, false, [] (test_rpc_proto& proto, test_rpc_proto::server& s, make_socket_fn make_socket) {
        return stream_test_func(proto, make_socket, true).then([] (stream_test_result r) {
            BOOST_REQUIRE(!r.client_source_closed &&
                    !r.server_source_closed &&
                    r.sink_exception &&
                    r.sink_close_exception &&
                    r.source_done_exception &&
                    r.server_done_exception &&
                    !r.client_stop_exception);
        });
    });
}


SEASTAR_TEST_CASE(test_stream_connection_error) {
    rpc::server_options so;
    so.streaming_domain = rpc::streaming_domain_type(1);
    return with_rpc_env({}, so, true, true, [] (test_rpc_proto& proto, test_rpc_proto::server& s, make_socket_fn make_socket) {
        return stream_test_func(proto, make_socket, false).then([] (stream_test_result r) {
            BOOST_REQUIRE(!r.client_source_closed &&
                    !r.server_source_closed &&
                    r.sink_exception &&
                    r.sink_close_exception &&
                    r.source_done_exception &&
                    r.server_done_exception &&
                    !r.client_stop_exception);
        });
    });
}

SEASTAR_TEST_CASE(test_rpc_scheduling) {
    return with_rpc_env({}, {}, true, false, [] (test_rpc_proto& proto, test_rpc_proto::server& s, make_socket_fn make_socket) {
        return seastar::async([&proto, make_socket] {
            test_rpc_proto::client c1(proto, {}, make_socket(), ipv4_addr());
            auto sg = create_scheduling_group("rpc", 100).get0();
            auto call = proto.register_handler(1, sg, [sg] () mutable {
                BOOST_REQUIRE(sg == current_scheduling_group());
                return make_ready_future<>();
            });
            call(c1).get();
            c1.stop().get();
        });
    });
}

SEASTAR_THREAD_TEST_CASE(test_rpc_scheduling_connection_based) {
    auto sg1 = create_scheduling_group("sg1", 100).get0();
    auto sg1_kill = defer([&] { destroy_scheduling_group(sg1).get(); });
    auto sg2 = create_scheduling_group("sg2", 100).get0();
    auto sg2_kill = defer([&] { destroy_scheduling_group(sg2).get(); });
    rpc::resource_limits limits;
    limits.isolate_connection = [sg1, sg2] (sstring cookie) {
        auto sg = current_scheduling_group();
        if (cookie == "sg1") {
            sg = sg1;
        } else if (cookie == "sg2") {
            sg = sg2;
        }
        rpc::isolation_config cfg;
        cfg.sched_group = sg;
        return cfg;
    };
    with_rpc_env(limits, {}, true, false, [sg1, sg2] (test_rpc_proto& proto, test_rpc_proto::server& s, make_socket_fn make_socket) {
        return async([&proto, make_socket, sg1, sg2] {
            rpc::client_options co1;
            co1.isolation_cookie = "sg1";
            test_rpc_proto::client c1(proto, co1, make_socket(), ipv4_addr());
            rpc::client_options co2;
            co2.isolation_cookie = "sg2";
            test_rpc_proto::client c2(proto, co2, make_socket(), ipv4_addr());
            auto call = proto.register_handler(1, [sg1, sg2] (int which) mutable {
                scheduling_group expected;
                if (which == 1) {
                    expected = sg1;
                } else if (which == 2) {
                    expected = sg2;
                }
                BOOST_REQUIRE(current_scheduling_group() == expected);
                return make_ready_future<>();
            });
            call(c1, 1).get();
            call(c2, 2).get();
            c1.stop().get();
            c2.stop().get();
        });
    }).get();
}

SEASTAR_THREAD_TEST_CASE(test_rpc_scheduling_connection_based_compatibility) {
    auto sg1 = create_scheduling_group("sg1", 100).get0();
    auto sg1_kill = defer([&] { destroy_scheduling_group(sg1).get(); });
    auto sg2 = create_scheduling_group("sg2", 100).get0();
    auto sg2_kill = defer([&] { destroy_scheduling_group(sg2).get(); });
    rpc::resource_limits limits;
    limits.isolate_connection = [sg1, sg2] (sstring cookie) {
        auto sg = current_scheduling_group();
        if (cookie == "sg1") {
            sg = sg1;
        } else if (cookie == "sg2") {
            sg = sg2;
        }
        rpc::isolation_config cfg;
        cfg.sched_group = sg;
        return cfg;
    };
    with_rpc_env(limits, {}, true, false, [sg1, sg2] (test_rpc_proto& proto, test_rpc_proto::server& s, make_socket_fn make_socket) {
        return async([&proto, make_socket, sg1, sg2] {
            rpc::client_options co1;
            co1.isolation_cookie = "sg1";
            test_rpc_proto::client c1(proto, co1, make_socket(), ipv4_addr());
            rpc::client_options co2;
            co2.isolation_cookie = "sg2";
            test_rpc_proto::client c2(proto, co2, make_socket(), ipv4_addr());
            // An old client, that doesn't have an isolation cookie
            rpc::client_options co3;
            test_rpc_proto::client c3(proto, co3, make_socket(), ipv4_addr());
            // A server that uses sg1 if the client is old
            auto call = proto.register_handler(1, sg1, [sg1, sg2] (int which) mutable {
                scheduling_group expected;
                if (which == 1) {
                    expected = sg1;
                } else if (which == 2) {
                    expected = sg2;
                }
                BOOST_REQUIRE(current_scheduling_group() == expected);
                return make_ready_future<>();
            });
            call(c1, 1).get();
            call(c2, 2).get();
            call(c3, 1).get();
            c1.stop().get();
            c2.stop().get();
            c3.stop().get();
        });
    }).get();
}
