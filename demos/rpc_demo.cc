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
 * Copyright 2015 Cloudius Systems
 */
#include <cmath>
#include <iostream>
#include <ranges>
#include <seastar/core/reactor.hh>
#include <seastar/core/app-template.hh>
#include <seastar/rpc/rpc.hh>
#include <seastar/core/sleep.hh>
#include <seastar/rpc/lz4_compressor.hh>
#include <seastar/util/log.hh>
#include <seastar/core/loop.hh>

using namespace seastar;

struct serializer {
};

template <typename T, typename Output>
inline
void write_arithmetic_type(Output& out, T v) {
    static_assert(std::is_arithmetic_v<T>, "must be arithmetic type");
    return out.write(reinterpret_cast<const char*>(&v), sizeof(T));
}

template <typename T, typename Input>
inline
T read_arithmetic_type(Input& in) {
    static_assert(std::is_arithmetic_v<T>, "must be arithmetic type");
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

namespace bpo = boost::program_options;
using namespace std::chrono_literals;

class mycomp : public rpc::compressor::factory {
    const sstring _name = "LZ4";
public:
    virtual const sstring& supported() const override {
        fmt::print("supported called\n");
        return _name;
    }
    virtual std::unique_ptr<rpc::compressor> negotiate(sstring feature, bool is_server) const override {
        fmt::print("negotiate called with {}\n", feature);
        return feature == _name ? std::make_unique<rpc::lz4_compressor>() : nullptr;
    }
};

int main(int ac, char** av) {
    app_template app;
    app.add_options()
                    ("port", bpo::value<uint16_t>()->default_value(10000), "RPC server port")
                    ("server", bpo::value<std::string>(), "Server address")
                    ("compress", bpo::value<bool>()->default_value(false), "Compress RPC traffic");
    std::cout << "start ";
    rpc::protocol<serializer> myrpc(serializer{});
    static std::unique_ptr<rpc::protocol<serializer>::server> server;
    static std::unique_ptr<rpc::protocol<serializer>::client> client;
    static double x = 30.0;

    static logger log("rpc_demo");
    myrpc.set_logger(&log);

    return app.run_deprecated(ac, av, [&] {
        auto&& config = app.configuration();
        uint16_t port = config["port"].as<uint16_t>();
        bool compress = config["compress"].as<bool>();
        static mycomp mc;
        auto test1 = myrpc.register_handler(1, [x = 0](int i) mutable { fmt::print("test1 count {:d} got {:d}\n", ++x, i); });
        auto test2 = myrpc.register_handler(2, [](int a, int b){ fmt::print("test2 got {:d} {:d}\n", a, b); return make_ready_future<int>(a+b); });
        auto test3 = myrpc.register_handler(3, [](double x){ fmt::print("test3 got {:f}\n", x); return std::make_unique<double>(sin(x)); });
        auto test4 = myrpc.register_handler(4, [](){ fmt::print("test4 throw!\n"); throw std::runtime_error("exception!"); });
        auto test5 = myrpc.register_handler(5, [](){ fmt::print("test5 no wait\n"); return rpc::no_wait; });
        auto test6 = myrpc.register_handler(6, [](const rpc::client_info& info, int x){ fmt::print("test6 client {}, {:d}\n", inet_ntoa(info.addr.as_posix_sockaddr_in().sin_addr), x); });
        auto test8 = myrpc.register_handler(8, [](){ fmt::print("test8 sleep for 2 sec\n"); return sleep(2s); });
        auto test13 = myrpc.register_handler(13, [](){ fmt::print("test13 sleep for 1 msec\n"); return sleep(1ms); });
        auto test_message_to_big = myrpc.register_handler(14, [](sstring payload){ fmt::print("test message to bit, should not get here"); });

        if (config.count("server")) {
            std::cout << "client" << std::endl;
            auto test7 = myrpc.make_client<long (long a, long b)>(7);
            auto test9 = myrpc.make_client<long (long a, long b)>(9); // do not send optional
            auto test9_1 = myrpc.make_client<long (long a, long b, int c)>(9); // send optional
            auto test9_2 = myrpc.make_client<long (long a, long b, int c, long d)>(9); // send more data than handler expects
            auto test10 = myrpc.make_client<long ()>(10); // receive less then replied
            auto test10_1 = myrpc.make_client<future<rpc::tuple<long, int>> ()>(10); // receive all
            auto test11 = myrpc.make_client<future<rpc::tuple<long, rpc::optional<int>>> ()>(11); // receive more then replied
            auto test12 = myrpc.make_client<void (int sleep_ms, sstring payload)>(12); // large payload vs. server limits
            auto test_nohandler = myrpc.make_client<void ()>(100000000); // non existing verb
            auto test_nohandler_nowait = myrpc.make_client<rpc::no_wait_type ()>(100000000); // non existing verb, no_wait call
            rpc::client_options co;
            if (compress) {
                co.compressor_factory = &mc;
            }

            client = std::make_unique<rpc::protocol<serializer>::client>(myrpc, co, ipv4_addr{config["server"].as<std::string>()});

            auto f = test8(*client, 1500ms).then_wrapped([](future<> f) {
                try {
                    f.get();
                    printf("test8 should not get here!\n");
                } catch (rpc::timeout_error&) {
                    printf("test8 timeout!\n");
                }
            });
            for (auto i = 0; i < 100; i++) {
                fmt::print("iteration={:d}\n", i);
                (void)test1(*client, 5).then([] (){ fmt::print("test1 ended\n");});
                (void)test2(*client, 1, 2).then([] (int r) { fmt::print("test2 got {:d}\n", r); });
                (void)test3(*client, x).then([](double x) { fmt::print("sin={:f}\n", x); });
                (void)test4(*client).then_wrapped([](future<> f) {
                    try {
                        f.get();
                        fmt::print("test4 your should not see this!\n");
                    } catch (std::runtime_error& x){
                        fmt::print("test4 {}\n", x.what());
                    }
                });
                (void)test5(*client).then([] { fmt::print("test5 no wait ended\n"); });
                (void)test6(*client, 1).then([] { fmt::print("test6 ended\n"); });
                (void)test7(*client, 5, 6).then([] (long r) { fmt::print("test7 got {:d}\n", r); });
                (void)test9(*client, 1, 2).then([] (long r) { fmt::print("test9 got {:d}\n", r); });
                (void)test9_1(*client, 1, 2, 3).then([] (long r) { fmt::print("test9.1 got {:d}\n", r); });
                (void)test9_2(*client, 1, 2, 3, 4).then([] (long r) { fmt::print("test9.2 got {:d}\n", r); });
                (void)test10(*client).then([] (long r) { fmt::print("test10 got {:d}\n", r); });
                (void)test10_1(*client).then([] (rpc::tuple<long, int> r) { fmt::print("test10_1 got {:d} and {:d}\n", std::get<0>(r), std::get<1>(r)); });
                (void)test11(*client).then([] (rpc::tuple<long, rpc::optional<int> > r) { fmt::print("test11 got {:d} and {:d}\n", std::get<0>(r), bool(std::get<1>(r))); });
                (void)test_nohandler(*client).then_wrapped([](future<> f) {
                    try {
                        f.get();
                        fmt::print("test_nohandler your should not see this!\n");
                    } catch (rpc::unknown_verb_error& x){
                        fmt::print("test_nohandle no such verb\n");
                    } catch (...) {
                        fmt::print("incorrect exception!\n");
                    }
                });
                (void)test_nohandler_nowait(*client);
                auto c = make_lw_shared<rpc::cancellable>();
                (void)test13(*client, *c).then_wrapped([](future<> f) {
                    try {
                        f.get();
                        fmt::print("test13 shold not get here\n");
                    } catch(rpc::canceled_error&) {
                        fmt::print("test13 canceled\n");
                    } catch(...) {
                        fmt::print("test13 wrong exception\n");
                    }
                });
                c->cancel();
                (void)test13(*client, *c).then_wrapped([](future<> f) {
                    try {
                        f.get();
                        fmt::print("test13 shold not get here\n");
                    } catch(rpc::canceled_error&) {
                        fmt::print("test13 canceled\n");
                    } catch(...) {
                        fmt::print("test13 wrong exception\n");
                    }
                });
                (void)sleep(500us).then([c] { c->cancel(); });
                (void)test_message_to_big(*client, uninitialized_string(10'000'001)).then_wrapped([](future<> f) {
                    try {
                        f.get();
                        fmt::print("test message to big shold not get here\n");
                    } catch(std::runtime_error& err) {
                        fmt::print("test message to big get error {}\n", err.what());
                    } catch(...) {
                        fmt::print("test message to big wrong exception\n");
                    }
                });
            }
            // delay a little for a time-sensitive test
            (void)sleep(400ms).then([test12] () mutable {
                // server is configured for 10MB max, throw 25MB worth of requests at it.
                auto now = rpc::rpc_clock_type::now();
                return parallel_for_each(std::views::iota(0, 25), [test12, now] (int idx) mutable {
                    return test12(*client, 100, uninitialized_string(1'000'000)).then([idx, now] {
                        auto later = rpc::rpc_clock_type::now();
                        auto delta = std::chrono::duration_cast<std::chrono::milliseconds>(later - now);
                        fmt::print("idx {:d} completed after {:d} ms\n", idx, delta.count());
                    });
                }).then([now] {
                    auto later = rpc::rpc_clock_type::now();
                    auto delta = std::chrono::duration_cast<std::chrono::milliseconds>(later - now);
                    fmt::print("test12 completed after {:d} ms (should be ~300)\n", delta.count());
                });
            });
            (void)f.finally([] {
                return sleep(1s).then([] {
                    return client->stop().then([] {
                        return engine().exit(0);
                    });
                });
            });
        } else {
            std::cout << "server on port " << port << std::endl;
            myrpc.register_handler(7, [](long a, long b) mutable {
                auto p = make_lw_shared<promise<>>();
                auto t = make_lw_shared<timer<>>();
                fmt::print("test7 got {:d} {:d}\n", a, b);
                auto f = p->get_future().then([a, b, t] {
                    fmt::print("test7 calc res\n");
                    return a - b;
                });
                t->set_callback([p = std::move(p)] () mutable { p->set_value(); });
                t->arm(1s);
                return f;
            });
            myrpc.register_handler(9, [] (long a, long b, rpc::optional<int> c) {
                long r = 2;
                fmt::print("test9 got {:d} {:d} ", a, b);
                if (c) {
                    fmt::print("{:d}", c.value());
                    r++;
                }
                fmt::print("\n");
                return r;
            });
            myrpc.register_handler(10, [] {
                fmt::print("test 10\n");
                return make_ready_future<rpc::tuple<long, int>>(rpc::tuple<long, int>(1, 2));
            });
            myrpc.register_handler(11, [] {
                fmt::print("test 11\n");
                return 1ul;
            });
            myrpc.register_handler(12, [] (int sleep_ms, sstring payload) {
                return sleep(std::chrono::milliseconds(sleep_ms)).then([] {
                    return make_ready_future<>();
                });
            });

            rpc::resource_limits limits;
            limits.bloat_factor = 1;
            limits.basic_request_size = 0;
            limits.max_memory = 10'000'000;
            rpc::server_options so;
            if (compress) {
                so.compressor_factory = &mc;
            }
            server = std::make_unique<rpc::protocol<serializer>::server>(myrpc, so, ipv4_addr{port}, limits);
        }
    });

}
