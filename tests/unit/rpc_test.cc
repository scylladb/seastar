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
#include <seastar/rpc/lz4_fragmented_compressor.hh>
#include <seastar/rpc/multi_algo_compressor_factory.hh>
#include <seastar/testing/test_case.hh>
#include <seastar/testing/thread_test_case.hh>
#include <seastar/testing/test_runner.hh>
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
                sink.flush().get();
                sink.close().get();
            });
            auto source_loop = seastar::async([source, &r] () mutable {
                while (!r.server_source_closed) {
                    auto data = source().get0();
                    if (data) {
                        r.server_sum += std::get<0>(*data);
                    } else {
                        r.server_source_closed = true;
                        try {
                          // check that reading after eos does not crash
                          // and throws correct exception
                           source().get();
                        } catch (rpc::stream_closed& ex) {
                          // expected
                        } catch (...) {
                           BOOST_FAIL("wrong exception on reading from a stream after eos");
                        }
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
            BOOST_REQUIRE(r.client_source_closed);
            BOOST_REQUIRE(r.server_source_closed);
            BOOST_REQUIRE(r.server_sum == 5050);
            BOOST_REQUIRE(!r.sink_exception);
            BOOST_REQUIRE(!r.sink_close_exception);
            BOOST_REQUIRE(!r.source_done_exception);
            BOOST_REQUIRE(!r.server_done_exception);
            BOOST_REQUIRE(!r.client_stop_exception);
        });
    });
}

SEASTAR_TEST_CASE(test_stream_stop_client) {
    rpc::server_options so;
    so.streaming_domain = rpc::streaming_domain_type(1);
    return with_rpc_env({}, so, true, false, [] (test_rpc_proto& proto, test_rpc_proto::server& s, make_socket_fn make_socket) {
        return stream_test_func(proto, make_socket, true).then([] (stream_test_result r) {
            BOOST_REQUIRE(!r.client_source_closed);
            BOOST_REQUIRE(!r.server_source_closed);
            BOOST_REQUIRE(r.sink_exception);
            BOOST_REQUIRE(r.sink_close_exception);
            BOOST_REQUIRE(r.source_done_exception);
            BOOST_REQUIRE(r.server_done_exception);
            BOOST_REQUIRE(!r.client_stop_exception);
        });
    });
}


SEASTAR_TEST_CASE(test_stream_connection_error) {
    rpc::server_options so;
    so.streaming_domain = rpc::streaming_domain_type(1);
    return with_rpc_env({}, so, true, true, [] (test_rpc_proto& proto, test_rpc_proto::server& s, make_socket_fn make_socket) {
        return stream_test_func(proto, make_socket, false).then([] (stream_test_result r) {
            BOOST_REQUIRE(!r.client_source_closed);
            BOOST_REQUIRE(!r.server_source_closed);
            BOOST_REQUIRE(r.sink_exception);
            BOOST_REQUIRE(r.sink_close_exception);
            BOOST_REQUIRE(r.source_done_exception);
            BOOST_REQUIRE(r.server_done_exception);
            BOOST_REQUIRE(!r.client_stop_exception);
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

void test_compressor(std::function<std::unique_ptr<seastar::rpc::compressor>()> compressor_factory) {
    using namespace seastar::rpc;

    auto linearize = [&] (const auto& buffer) {
        return seastar::visit(buffer.bufs,
            [] (const temporary_buffer<char>& buf) {
                return buf.clone();
            },
            [&] (const std::vector<temporary_buffer<char>>& bufs) {
                auto buf = temporary_buffer<char>(buffer.size);
                auto dst = buf.get_write();
                for (auto& b : bufs) {
                    dst = std::copy_n(b.get(), b.size(), dst);
                }
                return buf;
            }
        );
    };

    auto split_buffer = [&] (temporary_buffer<char> b, size_t chunk_size) {
        std::vector<temporary_buffer<char>> bufs;
        auto src = b.get();
        auto n = b.size();
        while (n) {
            auto this_chunk = std::min(chunk_size, n);
            bufs.emplace_back(this_chunk);
            std::copy_n(src, this_chunk, bufs.back().get_write());
            src += this_chunk;
            n -= this_chunk;
        }
        return bufs;
    };

    auto clone = [&] (const auto& buffer) {
        auto c = std::decay_t<decltype(buffer)>();
        c.size = buffer.size;
        c.bufs = seastar::visit(buffer.bufs,
            [] (const temporary_buffer<char>& buf) -> decltype(c.bufs) {
                return buf.clone();
            },
            [] (const std::vector<temporary_buffer<char>>& bufs) -> decltype(c.bufs) {
                std::vector<temporary_buffer<char>> c;
                c.reserve(bufs.size());
                for (auto& b : bufs) {
                    c.emplace_back(b.clone());
                }
                return SEASTAR_COPY_ELISION(c);
            }
        );
        return c;
    };

    auto compressor = compressor_factory();

    std::vector<std::tuple<sstring, size_t, snd_buf>> inputs;

    auto& eng = testing::local_random_engine;
    auto dist = std::uniform_int_distribution<char>();

    auto snd = snd_buf(1);
    *snd.front().get_write() = 'a';
    inputs.emplace_back("one byte, no headroom", 0, std::move(snd));

    snd = snd_buf(1);
    *snd.front().get_write() = 'a';
    inputs.emplace_back("one byte, 128k of headroom", 128 * 1024, std::move(snd));

    auto buf = temporary_buffer<char>(16 * 1024);
    std::fill_n(buf.get_write(), 16 * 1024, 'a');

    snd = snd_buf();
    snd.size = 16 * 1024;
    snd.bufs = buf.clone();
    inputs.emplace_back("single 16 kB buffer of \'a\'", 0, std::move(snd));

    buf = temporary_buffer<char>(16 * 1024);
    std::generate_n(buf.get_write(), 16 * 1024, [&] { return dist(eng); });

    snd = snd_buf();
    snd.size = 16 * 1024;
    snd.bufs = buf.clone();
    inputs.emplace_back("single 16 kB buffer of random", 0, std::move(snd));

    buf = temporary_buffer<char>(1 * 1024 * 1024);
    std::fill_n(buf.get_write(), 1 * 1024 * 1024, 'a');

    snd = snd_buf();
    snd.size = 1 * 1024 * 1024;
    snd.bufs = split_buffer(buf.clone(), 128 * 1024 - 128);
    inputs.emplace_back("1 MB buffer of \'a\' split into 128 kB - 128", 0, std::move(snd));

    snd = snd_buf();
    snd.size = 1 * 1024 * 1024;
    snd.bufs = split_buffer(buf.clone(), 128 * 1024);
    inputs.emplace_back("1 MB buffer of \'a\' split into 128 kB", 0, std::move(snd));

    buf = temporary_buffer<char>(1 * 1024 * 1024);
    std::generate_n(buf.get_write(), 1 * 1024 * 1024, [&] { return dist(eng); });

    snd = snd_buf();
    snd.size = 1 * 1024 * 1024;
    snd.bufs = split_buffer(buf.clone(), 128 * 1024);
    inputs.emplace_back("1 MB buffer of random split into 128 kB", 0, std::move(snd));

    buf = temporary_buffer<char>(1 * 1024 * 1024 + 1);
    std::fill_n(buf.get_write(), 1 * 1024 * 1024 + 1, 'a');

    snd = snd_buf();
    snd.size = 1 * 1024 * 1024 + 1;
    snd.bufs = split_buffer(buf.clone(), 128 * 1024);
    inputs.emplace_back("1 MB + 1B buffer of \'a\' split into 128 kB", 0, std::move(snd));

    buf = temporary_buffer<char>(1 * 1024 * 1024 + 1);
    std::generate_n(buf.get_write(), 1 * 1024 * 1024 + 1, [&] { return dist(eng); });

    snd = snd_buf();
    snd.size = 1 * 1024 * 1024 + 1;
    snd.bufs = split_buffer(buf.clone(), 128 * 1024);
    inputs.emplace_back("16 MB + 1 B buffer of random split into 128 kB", 0, std::move(snd));


    std::vector<std::tuple<sstring, std::function<rcv_buf(snd_buf)>>> transforms {
        { "identity", [] (snd_buf snd) {
            rcv_buf rcv;
            rcv.size = snd.size;
            rcv.bufs = std::move(snd.bufs);
            return rcv;
        } },
        { "linearized", [&linearize] (snd_buf snd) {
            rcv_buf rcv;
            rcv.size = snd.size;
            rcv.bufs = linearize(snd);
            return rcv;
        } },
        { "split 1 B", [&] (snd_buf snd) {
            rcv_buf rcv;
            rcv.size = snd.size;
            rcv.bufs = split_buffer(linearize(snd), 1);
            return rcv;
        } },
        { "split 129 B", [&] (snd_buf snd) {
            rcv_buf rcv;
            rcv.size = snd.size;
            rcv.bufs = split_buffer(linearize(snd), 129);
            return rcv;
        } },
        { "split 4 kB", [&] (snd_buf snd) {
            rcv_buf rcv;
            rcv.size = snd.size;
            rcv.bufs = split_buffer(linearize(snd), 4096);
            return rcv;
        } },
        { "split 4 kB - 128", [&] (snd_buf snd) {
            rcv_buf rcv;
            rcv.size = snd.size;
            rcv.bufs = split_buffer(linearize(snd), 4096 - 128);
            return rcv;
        } },
    };

    auto sanity_check = [&] (const auto& buffer) {
        auto actual_size = seastar::visit(buffer.bufs,
            [] (const temporary_buffer<char>& buf) {
                return buf.size();
            },
            [] (const std::vector<temporary_buffer<char>>& bufs) {
                return boost::accumulate(bufs, size_t(0), [] (size_t sz, const temporary_buffer<char>& buf) {
                    return sz + buf.size();
                });
            }
        );
        BOOST_CHECK_EQUAL(actual_size, buffer.size);
    };

    for (auto& in : inputs) {
        BOOST_TEST_MESSAGE("Input: " << std::get<0>(in));
        auto headroom = std::get<1>(in);
        auto compressed = compressor->compress(headroom, clone(std::get<2>(in)));
        sanity_check(compressed);

        // Remove headroom
        BOOST_CHECK_GE(compressed.size, headroom);
        compressed.size -= headroom;
        seastar::visit(compressed.bufs,
            [&] (temporary_buffer<char>& buf) {
                BOOST_CHECK_GE(buf.size(), headroom);
                buf.trim_front(headroom);
            },
            [&] (std::vector<temporary_buffer<char>>& bufs) {
                while (headroom) {
                    BOOST_CHECK(!bufs.empty());
                    auto to_remove = std::min(bufs.front().size(), headroom);
                    bufs.front().trim_front(to_remove);
                    if (bufs.front().empty() && bufs.size() > 1) {
                        bufs.erase(bufs.begin());
                    }
                    headroom -= to_remove;
                }
            }
        );

        auto in_l = linearize(std::get<2>(in));

        for (auto& t : transforms) {
            BOOST_TEST_MESSAGE("  Transform: " << std::get<0>(t));
            auto received = std::get<1>(t)(clone(compressed));

            auto decompressed = compressor->decompress(std::move(received));
            sanity_check(decompressed);

            BOOST_CHECK_EQUAL(decompressed.size, std::get<2>(in).size);

            auto out_l = linearize(decompressed);

            BOOST_CHECK_EQUAL(in_l.size(), out_l.size());
            BOOST_CHECK(in_l == out_l);
        }
    }
}

SEASTAR_THREAD_TEST_CASE(test_lz4_compressor) {
    test_compressor([] { return std::make_unique<rpc::lz4_compressor>(); });
}

SEASTAR_THREAD_TEST_CASE(test_lz4_fragmented_compressor) {
    test_compressor([] { return std::make_unique<rpc::lz4_fragmented_compressor>(); });
}

// Test reproducing issue #671: If timeout is time_point::max(), translating
// it to relative timeout in the sender and then back in the receiver, when
// these calculations happen across a millisecond boundary, overflowed the
// integer and mislead the receiver to think the requested timeout was
// negative, and cause it drop its response, so the RPC call never completed.
SEASTAR_TEST_CASE(test_max_absolute_timeout) {
    // The typical failure of this test is a hang. So we use semaphore to
    // stop the test either when it succeeds, or after a long enough hang.
    auto success = make_lw_shared<bool>(false);
    auto done = make_lw_shared<semaphore>(0);
    auto abrt = make_lw_shared<abort_source>();
    (void) seastar::sleep_abortable(std::chrono::seconds(3), *abrt).then([done, success] {
        done->signal(1);
    }).handle_exception([] (std::exception_ptr) {});
    (void) with_rpc_env({}, rpc::server_options(), true, false, [] (test_rpc_proto& proto, test_rpc_proto::server& s, make_socket_fn make_socket) {
        return seastar::async([&proto, make_socket] {
            rpc::client_options co;
            co.send_timeout_data = 1;
            test_rpc_proto::client c1(proto, co, make_socket(), ipv4_addr());
            auto sum = proto.register_handler(1, [](int a, int b) {
                return make_ready_future<int>(a+b);
            });
            // The bug only reproduces if the calculation done on the sender
            // and receiver sides, happened across a millisecond boundary.
            // We can't control when it happens, so we just need to loop many
            // times, at least many milliseconds, to increase the probability
            // that we catch the bug. Experimentally, if we loop for 200ms, we
            // catch the bug in #671 virtually every time.
            auto until = seastar::lowres_clock::now() + std::chrono::milliseconds(200);
            while (seastar::lowres_clock::now() <= until) {
                auto result = sum(c1, rpc::rpc_clock_type::time_point::max(), 2, 3).get0();
                BOOST_REQUIRE_EQUAL(result, 2 + 3);
            }
            c1.stop().get();
        });
    }).then([success, done, abrt] {
        *success = true;
        abrt->request_abort();
        done->signal();
    });
    return done->wait().then([done, success] {
        BOOST_REQUIRE(*success);
    });
}

// Similar to the above test: Test that a relative timeout duration::max()
// also works, and again doesn't cause the timeout wrapping around to the
// past and causing dropped responses.
SEASTAR_TEST_CASE(test_max_relative_timeout) {
    // The typical failure of this test is a hang. So we use semaphore to
    // stop the test either when it succeeds, or after a long enough hang.
    auto success = make_lw_shared<bool>(false);
    auto done = make_lw_shared<semaphore>(0);
    auto abrt = make_lw_shared<abort_source>();
    (void) seastar::sleep_abortable(std::chrono::seconds(3), *abrt).then([done, success] {
        done->signal(1);
    }).handle_exception([] (std::exception_ptr) {});
    (void) with_rpc_env({}, rpc::server_options(), true, false, [] (test_rpc_proto& proto, test_rpc_proto::server& s, make_socket_fn make_socket) {
        return seastar::async([&proto, make_socket] {
            rpc::client_options co;
            co.send_timeout_data = 1;
            test_rpc_proto::client c1(proto, co, make_socket(), ipv4_addr());
            auto sum = proto.register_handler(1, [](int a, int b) {
                return make_ready_future<int>(a+b);
            });
            // The following call used to always hang, when max()+now()
            // overflowed and appeared to be a negative timeout.
            auto result = sum(c1, rpc::rpc_clock_type::duration::max(), 2, 3).get0();
            BOOST_REQUIRE_EQUAL(result, 2 + 3);
            c1.stop().get();
        });
    }).then([success, done, abrt] {
        *success = true;
        abrt->request_abort();
        done->signal();
    });
    return done->wait().then([done, success] {
        BOOST_REQUIRE(*success);
    });
}
