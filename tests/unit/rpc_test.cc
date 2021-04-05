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
#include <seastar/core/distributed.hh>
#include <seastar/core/loop.hh>
#include <seastar/util/defer.hh>
#include <seastar/util/log.hh>
#include <seastar/util/closeable.hh>
#include <seastar/util/noncopyable_function.hh>

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
    sstring ret = uninitialized_string(size);
    in.read(ret.data(), size);
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
    virtual void set_reuseaddr(bool reuseaddr) override {}
    virtual bool get_reuseaddr() const override { return false; };
    virtual void shutdown() override {
        if (_connect) {
            _socket.shutdown();
        } else {
            _p.set_exception(std::make_exception_ptr(std::system_error(ECONNABORTED, std::system_category())));
        }
    }
};

struct rpc_test_config {
    rpc::resource_limits resource_limits = {};
    rpc::server_options server_options = {};
    bool connect = true;
    bool inject_error = false;
};

template<typename MsgType = int>
class rpc_test_env {
    struct rpc_test_service {
        test_rpc_proto _proto;
        test_rpc_proto::server _server;
        std::vector<MsgType> _handlers;

        rpc_test_service() = delete;
        explicit rpc_test_service(const rpc_test_config& cfg, loopback_connection_factory& lcf)
                : _proto(serializer())
                , _server(_proto, cfg.server_options, lcf.get_server_socket(), cfg.resource_limits)
        { }

        test_rpc_proto& proto() {
            return _proto;
        }

        test_rpc_proto::server& server() {
            return _server;
        }

        future<> stop() {
            return parallel_for_each(_handlers, [this] (auto t) {
                return proto().unregister_handler(t);
            }).finally([this] {
                return server().stop();
            });
        }

        template<typename Func>
        auto register_handler(MsgType t, scheduling_group sg, Func func) {
            _handlers.emplace_back(t);
            return proto().register_handler(t, sg, std::move(func));
        }

        future<> unregister_handler(MsgType t) {
            auto it = std::find(_handlers.begin(), _handlers.end(), t);
            assert(it != _handlers.end());
            _handlers.erase(it);
            return proto().unregister_handler(t);
        }
    };

    rpc_test_config _cfg;
    loopback_connection_factory _lcf;
    std::unique_ptr<sharded<rpc_test_service>> _service;

public:
    rpc_test_env() = delete;
    explicit rpc_test_env(rpc_test_config cfg)
        : _cfg(cfg), _service(std::make_unique<sharded<rpc_test_service>>())
    {
    }

    using test_fn = std::function<future<> (rpc_test_env<MsgType>& env)>;
    static future<> do_with(rpc_test_config cfg, test_fn&& func) {
        return seastar::do_with(rpc_test_env(cfg), [func] (rpc_test_env<MsgType>& env) {
            return env.start().then([&env, func] {
                return func(env);
            }).finally([&env] {
                return env.stop();
            });
        });
    }

    using thread_test_fn = std::function<void (rpc_test_env<MsgType>& env)>;
    static future<> do_with_thread(rpc_test_config cfg, thread_test_fn&& func) {
        return do_with(std::move(cfg), [func] (rpc_test_env<MsgType>& env) {
            return seastar::async([&env, func] {
                func(env);
            });
        });
    }

    using thread_test_fn_with_client = std::function<void (rpc_test_env<MsgType>& env, test_rpc_proto::client& cl)>;
    static future<> do_with_thread(rpc_test_config cfg, rpc::client_options co, thread_test_fn_with_client&& func) {
        return do_with(std::move(cfg), [func, co = std::move(co)] (rpc_test_env<MsgType>& env) {
            return seastar::async([&env, func, co = std::move(co)] {
                test_rpc_proto::client cl(env.proto(), co, env.make_socket(), ipv4_addr());
                auto stop = deferred_stop(cl);
                func(env, cl);
            });
        });
    }

    static future<> do_with_thread(rpc_test_config cfg, thread_test_fn_with_client&& func) {
        return do_with_thread(std::move(cfg), rpc::client_options(), std::move(func));
    }

    auto make_socket() {
        return seastar::socket(std::make_unique<rpc_socket_impl>(_lcf, _cfg.connect, _cfg.inject_error));
    };

    test_rpc_proto& proto() {
        return local_service().proto();
    }

    test_rpc_proto::server& server() {
        return local_service().server();
    }

    template<typename Func>
    future<> register_handler(MsgType t, scheduling_group sg, Func func) {
        return _service->invoke_on_all([t, func = std::move(func), sg] (rpc_test_service& s) mutable {
            s.register_handler(t, sg, std::move(func));
        });
    }

    template<typename Func>
    future<> register_handler(MsgType t, Func func) {
        return register_handler(t, scheduling_group(), std::move(func));
    }

    future<> unregister_handler(MsgType t) {
        return _service->invoke_on_all([t] (rpc_test_service& s) mutable {
            return s.unregister_handler(t);
        });
    }

private:
    rpc_test_service& local_service() {
        return _service->local();

    }

    future<> start() {
        return _service->start(std::cref(_cfg), std::ref(_lcf));
    }

    future<> stop() {
        return _service->stop().then([this] {
            return _lcf.destroy_all_shards();
        });
    }
};

struct cfactory : rpc::compressor::factory {
    mutable int use_compression = 0;
    const sstring name;
    cfactory(sstring name_ = "LZ4") : name(std::move(name_)) {}
    const sstring& supported() const override {
        return name;
    }
    class mylz4 : public rpc::lz4_compressor {
        sstring _name;
    public:
        mylz4(const sstring& n) : _name(n) {}
        sstring name() const override {
            return _name;
        }
    };
    std::unique_ptr<rpc::compressor> negotiate(sstring feature, bool is_server) const override {
        if (feature == name) {
            use_compression++;
            return std::make_unique<mylz4>(name);
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
            rpc_test_config cfg;
            cfg.server_options = so;
            auto f = rpc_test_env<>::do_with_thread(cfg, co, [] (rpc_test_env<>& env, test_rpc_proto::client& c1) {
                env.register_handler(1, [](int a, int b) {
                    return make_ready_future<int>(a+b);
                }).get();
                auto sum = env.proto().make_client<int (int, int)>(1);
                auto result = sum(c1, 2, 3).get0();
                BOOST_REQUIRE_EQUAL(result, 2 + 3);
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
    rpc_test_config cfg;
    cfg.server_options = so;
    return rpc_test_env<>::do_with_thread(cfg, co, [] (rpc_test_env<>& env, test_rpc_proto::client& c1) {
        env.register_handler(1, [](int a, int b) {
            return make_ready_future<int>(a+b);
        }).get();
        auto sum = env.proto().make_client<int (int, int)>(1);
        auto result = sum(c1, 2, 3).get0();
        BOOST_REQUIRE_EQUAL(result, 2 + 3);
    }).finally([factory1 = std::move(factory1), factory2 = std::move(factory2)] {
        BOOST_REQUIRE_EQUAL(factory1->use_compression, 0);
        BOOST_REQUIRE_EQUAL(factory2->use_compression, 2);
    });
}

SEASTAR_TEST_CASE(test_rpc_connect_abort) {
    rpc_test_config cfg;
    cfg.connect = false;
    return rpc_test_env<>::do_with_thread(cfg, [] (rpc_test_env<>& env) {
        test_rpc_proto::client c1(env.proto(), {}, env.make_socket(), ipv4_addr());
        env.register_handler(1, []() { return make_ready_future<>(); }).get();
        auto f = env.proto().make_client<void ()>(1);
        c1.stop().get0();
        try {
            f(c1).get0();
            BOOST_REQUIRE(false);
        } catch (...) {}
    });
}

SEASTAR_TEST_CASE(test_rpc_cancel) {
    using namespace std::chrono_literals;
    return rpc_test_env<>::do_with_thread(rpc_test_config(), [] (rpc_test_env<>& env, test_rpc_proto::client& c1) {
        bool rpc_executed = false;
        int good = 0;
        promise<> handler_called;
        future<> f_handler_called = handler_called.get_future();
        env.register_handler(1, [&rpc_executed, &handler_called] {
            handler_called.set_value(); rpc_executed = true; return sleep(1ms);
        }).get();
        auto call = env.proto().make_client<void ()>(1);
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
        BOOST_REQUIRE_EQUAL(good, 11);
    });
}

SEASTAR_TEST_CASE(test_message_to_big) {
    rpc_test_config cfg;
    cfg.resource_limits = {0, 1, 100};
    return rpc_test_env<>::do_with_thread(cfg, [] (rpc_test_env<>& env, test_rpc_proto::client& c) {
        bool good = true;
        env.register_handler(1, [&] (sstring payload) mutable {
            good = false;
        }).get();
        auto call = env.proto().make_client<void (sstring)>(1);
        try {
            call(c, uninitialized_string(101)).get();
            good = false;
        } catch(std::runtime_error& err) {
        } catch(...) {
            good = false;
        }
        BOOST_REQUIRE_EQUAL(good, true);
    });
}

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

future<stream_test_result> stream_test_func(rpc_test_env<>& env, bool stop_client, bool expect_connection_error = false) {
    return seastar::async([&env, stop_client, expect_connection_error] {
        stream_test_result r;
        test_rpc_proto::client c(env.proto(), {}, env.make_socket(), ipv4_addr());
        future<> server_done = make_ready_future();
        env.register_handler(1, [&](int i, rpc::source<int> source) {
            BOOST_REQUIRE_EQUAL(i, 666);
            auto sink = source.make_sink<serializer, sstring>();
            auto sink_loop = seastar::async([sink] () mutable {
                for (auto i = 0; i < 100; i++) {
                    sink("seastar").get();
                    sleep(std::chrono::milliseconds(1)).get();
                }
            }).finally([sink] () mutable {
                return sink.flush();
            }).finally([sink] () mutable {
                return sink.close();
            }).finally([sink] {});

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
        }).get();
        auto call = env.proto().make_client<rpc::source<sstring> (int, rpc::sink<int>)>(1);
        auto x = [&] {
            try {
                return c.make_stream_sink<serializer, int>(env.make_socket()).get0();
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
        // With a connection error sink() will eventually fail, but we
        // cannot guarantee when.
        int max = expect_connection_error ? std::numeric_limits<int>::max()  : 101;
        for (int i = 1; i < max; i++) {
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
    rpc_test_config cfg;
    cfg.server_options = so;
    return rpc_test_env<>::do_with(cfg, [] (rpc_test_env<>& env) {
        return stream_test_func(env, false).then([] (stream_test_result r) {
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
    rpc_test_config cfg;
    cfg.server_options = so;
    return rpc_test_env<>::do_with(cfg, [] (rpc_test_env<>& env) {
        return stream_test_func(env, true).then([] (stream_test_result r) {
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
    rpc_test_config cfg;
    cfg.server_options = so;
    cfg.inject_error = true;
    return rpc_test_env<>::do_with(cfg, [] (rpc_test_env<>& env) {
        return stream_test_func(env, false, true).then([] (stream_test_result r) {
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
    return rpc_test_env<>::do_with_thread(rpc_test_config(), [] (rpc_test_env<>& env, test_rpc_proto::client& c1) {
        auto sg = create_scheduling_group("rpc", 100).get0();
        env.register_handler(1, sg, [] () {
            return make_ready_future<unsigned>(internal::scheduling_group_index(current_scheduling_group()));
        }).get();
        auto call_sg_id = env.proto().make_client<unsigned ()>(1);
        auto id = call_sg_id(c1).get0();
        BOOST_REQUIRE(id == internal::scheduling_group_index(sg));
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
    rpc_test_config cfg;
    cfg.resource_limits = limits;
    rpc_test_env<>::do_with_thread(cfg, [sg1, sg2] (rpc_test_env<>& env) {
        rpc::client_options co1;
        co1.isolation_cookie = "sg1";
        test_rpc_proto::client c1(env.proto(), co1, env.make_socket(), ipv4_addr());
        rpc::client_options co2;
        co2.isolation_cookie = "sg2";
        test_rpc_proto::client c2(env.proto(), co2, env.make_socket(), ipv4_addr());
        env.register_handler(1, [] {
            return make_ready_future<unsigned>(internal::scheduling_group_index(current_scheduling_group()));
        }).get();
        auto call_sg_id = env.proto().make_client<unsigned ()>(1);
        unsigned id;
        id = call_sg_id(c1).get0();
        BOOST_REQUIRE(id == internal::scheduling_group_index(sg1));
        id = call_sg_id(c2).get0();
        BOOST_REQUIRE(id == internal::scheduling_group_index(sg2));
        c1.stop().get();
        c2.stop().get();
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
    rpc_test_config cfg;
    cfg.resource_limits = limits;
    rpc_test_env<>::do_with_thread(cfg, [sg1, sg2] (rpc_test_env<>& env) {
        rpc::client_options co1;
        co1.isolation_cookie = "sg1";
        test_rpc_proto::client c1(env.proto(), co1, env.make_socket(), ipv4_addr());
        rpc::client_options co2;
        co2.isolation_cookie = "sg2";
        test_rpc_proto::client c2(env.proto(), co2, env.make_socket(), ipv4_addr());
        // An old client, that doesn't have an isolation cookie
        rpc::client_options co3;
        test_rpc_proto::client c3(env.proto(), co3, env.make_socket(), ipv4_addr());
        // A server that uses sg1 if the client is old
        env.register_handler(1, sg1, [] () {
            return make_ready_future<unsigned>(internal::scheduling_group_index(current_scheduling_group()));
        }).get();
        auto call_sg_id = env.proto().make_client<unsigned ()>(1);
        unsigned id;
        id = call_sg_id(c1).get0();
        BOOST_REQUIRE(id == internal::scheduling_group_index(sg1));
        id = call_sg_id(c2).get0();
        BOOST_REQUIRE(id == internal::scheduling_group_index(sg2));
        id = call_sg_id(c3).get0();
        BOOST_REQUIRE(id == internal::scheduling_group_index(sg1));
        c1.stop().get();
        c2.stop().get();
        c3.stop().get();
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
                return c;
            }
        );
        return c;
    };

    auto compressor = compressor_factory();

    std::vector<noncopyable_function<std::tuple<sstring, size_t, snd_buf> ()>> inputs;

    auto add_input = [&] (auto func_returning_tuple) {
        inputs.emplace_back(std::move(func_returning_tuple));
    };

    auto& eng = testing::local_random_engine;
    auto dist = std::uniform_int_distribution<char>();

    auto snd = snd_buf(1);
    *snd.front().get_write() = 'a';
    add_input([snd = std::move(snd)] () mutable { return std::tuple(sstring("one byte, no headroom"), size_t(0), std::move(snd)); });

    snd = snd_buf(1);
    *snd.front().get_write() = 'a';
    add_input([snd = std::move(snd)] () mutable { return std::tuple(sstring("one byte, 128k of headroom"), size_t(128 * 1024), std::move(snd)); });

    auto gen_fill = [&](size_t s, sstring msg, std::optional<size_t> split = {}) {
        auto buf = temporary_buffer<char>(s);
        std::fill_n(buf.get_write(), s, 'a');

        auto snd = snd_buf();
        snd.size = s;
        if (split) {
            snd.bufs = split_buffer(buf.clone(), *split);
        } else {
            snd.bufs = buf.clone();
        }
        return std::tuple(msg, size_t(0), std::move(snd));
    };


    add_input(std::bind(gen_fill, 16 * 1024, "single 16 kB buffer of \'a\'"));

    auto gen_rand = [&](size_t s, sstring msg, std::optional<size_t> split = {}) {
        auto buf = temporary_buffer<char>(s);
        std::generate_n(buf.get_write(), s, [&] { return dist(eng); });

        auto snd = snd_buf();
        snd.size = s;
        if (split) {
            snd.bufs = split_buffer(buf.clone(), *split);
        } else {
            snd.bufs = buf.clone();
        }
        return std::tuple(msg, size_t(0), std::move(snd));
    };

    add_input(std::bind(gen_rand, 16 * 1024, "single 16 kB buffer of random"));

    auto gen_text = [&](size_t s, sstring msg, std::optional<size_t> split = {}) {
        static const std::string_view text = "The quick brown fox wants bananas for his long term health but sneaks bacon behind his wife's back. ";

        auto buf = temporary_buffer<char>(s);
        size_t n = 0;
        while (n < s) {
            auto rem = std::min(s - n, text.size());
            std::copy(text.data(), text.data() + rem, buf.get_write() + n);
            n += rem;
        }

        auto snd = snd_buf();
        snd.size = s;
        if (split) {
            snd.bufs = split_buffer(buf.clone(), *split);
        } else {
            snd.bufs = buf.clone();
        }
        return std::tuple(msg, size_t(0), std::move(snd));
    };


    for (auto s : { 1, 4 }) {
        for (auto ss : { 32, 64, 128, 48, 56, 246, 511 }) {
            add_input(std::bind(gen_fill, s * 1024 * 1024, format("{} MB buffer of \'a\' split into {} kB - {}", s, ss, ss), ss * 1024 - ss));
            add_input(std::bind(gen_fill, s * 1024 * 1024, format("{} MB buffer of \'a\' split into {} kB", s, ss), ss * 1024));
            add_input(std::bind(gen_rand, s * 1024 * 1024, format("{} MB buffer of random split into {} kB", s, ss), ss * 1024));

            add_input(std::bind(gen_fill, s * 1024 * 1024 + 1, format("{} MB + 1B buffer of \'a\' split into {} kB", s, ss), ss * 1024));
            add_input(std::bind(gen_rand, s * 1024 * 1024 + 1, format("{} MB + 1B buffer of random split into {} kB", s, ss), ss * 1024));
        }

        for (auto ss : { 128, 246, 511, 3567, 2*1024, 8*1024 }) {
            add_input(std::bind(gen_fill, s * 1024 * 1024, format("{} MB buffer of \'a\' split into {} B", s, ss), ss));
            add_input(std::bind(gen_rand, s * 1024 * 1024, format("{} MB buffer of random split into {} B", s, ss), ss));
            add_input(std::bind(gen_text, s * 1024 * 1024, format("{} MB buffer of text split into {} B", s, ss), ss));
            add_input(std::bind(gen_fill, s * 1024 * 1024 - ss, format("{} MB - {}B buffer of \'a\' split into {} B", s, ss, ss), ss));
            add_input(std::bind(gen_rand, s * 1024 * 1024 - ss, format("{} MB - {}B buffer of random split into {} B", s, ss, ss), ss));
            add_input(std::bind(gen_text, s * 1024 * 1024 - ss, format("{} MB - {}B buffer of random split into {} B", s, ss, ss), ss));
        }
    }

    for (auto s : { 64*1024 + 5670, 16*1024 + 3421, 32*1024 - 321 }) {
        add_input(std::bind(gen_fill, s, format("{} bytes buffer of \'a\'", s)));
        add_input(std::bind(gen_rand, s, format("{} bytes buffer of random", s)));
        add_input(std::bind(gen_text, s, format("{} bytes buffer of text", s)));
    }

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

    for (auto& in_func : inputs) {
        auto in = in_func();
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
    rpc::client_options co;
    co.send_timeout_data = 1;
    (void)rpc_test_env<>::do_with_thread(rpc_test_config(), co, [] (rpc_test_env<>& env, test_rpc_proto::client& c1) {
        env.register_handler(1, [](int a, int b) {
            return make_ready_future<int>(a+b);
        }).get();
        auto sum = env.proto().make_client<int (int, int)>(1);
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
    rpc::client_options co;
    co.send_timeout_data = 1;
    (void)rpc_test_env<>::do_with_thread(rpc_test_config(), co, [] (rpc_test_env<>& env, test_rpc_proto::client& c1) {
        env.register_handler(1, [](int a, int b) {
            return make_ready_future<int>(a+b);
        }).get();
        auto sum = env.proto().make_client<int (int, int)>(1);
        // The following call used to always hang, when max()+now()
        // overflowed and appeared to be a negative timeout.
        auto result = sum(c1, rpc::rpc_clock_type::duration::max(), 2, 3).get0();
        BOOST_REQUIRE_EQUAL(result, 2 + 3);
    }).then([success, done, abrt] {
        *success = true;
        abrt->request_abort();
        done->signal();
    });
    return done->wait().then([done, success] {
        BOOST_REQUIRE(*success);
    });
}

SEASTAR_TEST_CASE(test_rpc_tuple) {
    return rpc_test_env<>::do_with_thread(rpc_test_config(), [] (rpc_test_env<>& env, test_rpc_proto::client& c1) {
        env.register_handler(1, [] () {
            return make_ready_future<rpc::tuple<int, long>>(rpc::tuple<int, long>(1, 0x7'0000'0000L));
        }).get();
        auto f1 = env.proto().make_client<rpc::tuple<int, long> ()>(1);
        auto result = f1(c1).get0();
        BOOST_REQUIRE_EQUAL(std::get<0>(result), 1);
        BOOST_REQUIRE_EQUAL(std::get<1>(result), 0x7'0000'0000L);
    });
}

SEASTAR_TEST_CASE(test_rpc_nonvariadic_client_variadic_server) {
    return rpc_test_env<>::do_with_thread(rpc_test_config(), [] (rpc_test_env<>& env, test_rpc_proto::client& c1) {
        // Server is variadic
        env.register_handler(1, [] () {
            return make_ready_future<rpc::tuple<int, long>>(rpc::tuple(1, 0x7'0000'0000L));
        }).get();
        // Client is non-variadic
        auto f1 = env.proto().make_client<future<rpc::tuple<int, long>> ()>(1);
        auto result = f1(c1).get0();
        BOOST_REQUIRE_EQUAL(std::get<0>(result), 1);
        BOOST_REQUIRE_EQUAL(std::get<1>(result), 0x7'0000'0000L);
    });
}

SEASTAR_TEST_CASE(test_rpc_variadic_client_nonvariadic_server) {
    return rpc_test_env<>::do_with_thread(rpc_test_config(), [] (rpc_test_env<>& env, test_rpc_proto::client& c1) {
        // Server is nonvariadic
        env.register_handler(1, [] () {
            return make_ready_future<rpc::tuple<int, long>>(rpc::tuple<int, long>(1, 0x7'0000'0000L));
        }).get();
        // Client is variadic
        auto f1 = env.proto().make_client<future<rpc::tuple<int, long>> ()>(1);
        auto result = f1(c1).get0();
        BOOST_REQUIRE_EQUAL(std::get<0>(result), 1);
        BOOST_REQUIRE_EQUAL(std::get<1>(result), 0x7'0000'0000L);
    });
}

SEASTAR_TEST_CASE(test_handler_registration) {
    rpc_test_config cfg;
    cfg.connect = false;
    return rpc_test_env<>::do_with_thread(cfg, [] (rpc_test_env<>& env) {
        auto& proto = env.proto();

        // new proto must be empty
        BOOST_REQUIRE(!proto.has_handlers());

        // non-existing handler should not be found
        BOOST_REQUIRE(!proto.has_handler(1));

        // unregistered non-existing handler should return ready future
        auto fut = proto.unregister_handler(1);
        BOOST_REQUIRE(fut.available() && !fut.failed());
        fut.get();

        // existing handler should be found
        auto handler = [] () { return make_ready_future<>(); };
        proto.register_handler(1, handler);
        BOOST_REQUIRE(proto.has_handler(1));

        // cannot register handler if already registered
        BOOST_REQUIRE_THROW(proto.register_handler(1, handler), std::runtime_error);

        // unregistered handler should not be found
        proto.unregister_handler(1).get();
        BOOST_REQUIRE(!proto.has_handler(1));

        // re-registering a handler should succeed
        proto.register_handler(1, handler);
        BOOST_REQUIRE(proto.has_handler(1));

        // proto with handlers must not be empty
        BOOST_REQUIRE(proto.has_handlers());
    });
}

SEASTAR_TEST_CASE(test_unregister_handler) {
    using namespace std::chrono_literals;
    return rpc_test_env<>::do_with_thread(rpc_test_config(), [] (rpc_test_env<>& env, test_rpc_proto::client& c1) {
        promise<> handler_called;
        future<> f_handler_called = handler_called.get_future();
        bool rpc_executed = false;
        bool rpc_completed = false;

        auto reset_state = [&f_handler_called, &rpc_executed, &rpc_completed] {
            if (f_handler_called.available()) {
                f_handler_called.get();
            }
            rpc_executed = false;
            rpc_completed = false;
        };

        auto get_handler = [&handler_called, &rpc_executed, &rpc_completed] {
            return [&handler_called, &rpc_executed, &rpc_completed] {
                handler_called.set_value();
                rpc_executed = true;
                return sleep(1ms).then([&rpc_completed] {
                    rpc_completed = true;
                });
            };
        };

        // handler should not run if unregistered before being called
        env.register_handler(1, get_handler()).get();
        env.unregister_handler(1).get();
        BOOST_REQUIRE(!f_handler_called.available());
        BOOST_REQUIRE(!rpc_executed);
        BOOST_REQUIRE(!rpc_completed);

        // verify normal execution path
        env.register_handler(1, get_handler()).get();
        auto call = env.proto().make_client<void ()>(1);
        call(c1).get();
        BOOST_REQUIRE(f_handler_called.available());
        BOOST_REQUIRE(rpc_executed);
        BOOST_REQUIRE(rpc_completed);
        reset_state();

        // call should fail after handler is unregistered
        env.unregister_handler(1).get();
        try {
            call(c1).get();
            BOOST_REQUIRE(false);
        } catch (rpc::unknown_verb_error&) {
            // expected
        } catch (...) {
            std::cerr << "call failed in an unexpected way: " << std::current_exception() << std::endl;
            BOOST_REQUIRE(false);
        }
        BOOST_REQUIRE(!f_handler_called.available());
        BOOST_REQUIRE(!rpc_executed);
        BOOST_REQUIRE(!rpc_completed);

        // unregistered is allowed while call is in flight
        auto delayed_unregister = [&env] {
            return sleep(500us).then([&env] {
                return env.unregister_handler(1);
            });
        };

        env.register_handler(1, get_handler()).get();
        try {
            when_all_succeed(call(c1), delayed_unregister()).get();
            reset_state();
        } catch (rpc::unknown_verb_error&) {
            // expected
        } catch (...) {
            std::cerr << "call failed in an unexpected way: " << std::current_exception() << std::endl;
            BOOST_REQUIRE(false);
        }
    });
}

SEASTAR_TEST_CASE(test_loggers) {
    static seastar::logger log("dummy");
    log.set_level(log_level::debug);
    return rpc_test_env<>::do_with_thread(rpc_test_config(), [] (rpc_test_env<>& env, test_rpc_proto::client& c1) {
        socket_address dummy_addr;
        auto& proto = env.proto();
        auto& logger = proto.get_logger();
        logger(dummy_addr, "Hello0");
        logger(dummy_addr, log_level::debug, "Hello1");
        proto.set_logger(&log);
        logger(dummy_addr, "Hello2");
        logger(dummy_addr, log_level::debug, "Hello3");
        // We *want* to test the deprecated API, don't spam warnings about it.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
        proto.set_logger([] (const sstring& str) {
            log.info("Test: {}", str);
        });
#pragma GCC diagnostic pop
        logger(dummy_addr, "Hello4");
        logger(dummy_addr, log_level::debug, "Hello5");
        proto.set_logger(nullptr);
        logger(dummy_addr, "Hello6");
        logger(dummy_addr, log_level::debug, "Hello7");
    });
}

static_assert(std::is_same_v<decltype(rpc::tuple(1U, 1L)), rpc::tuple<unsigned, long>>, "rpc::tuple deduction guid not working");
