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
 * Copyright (C) 2019 Elazar Leibovich
 */

#include <seastar/core/reactor.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/app-template.hh>
#include <seastar/core/print.hh>
#include <seastar/core/memory.hh>
#include <seastar/core/byteorder.hh>
#include <seastar/core/alien.hh>
#include <seastar/core/smp.hh>
#include <seastar/util/assert.hh>
#include <seastar/util/std-compat.hh>
#include <seastar/util/later.hh>
#include <seastar/util/defer.hh>
#include <seastar/testing/test_case.hh>
#include <seastar/testing/thread_test_case.hh>
#include <seastar/testing/random.hh>
#include <seastar/core/loop.hh>
#include <seastar/core/with_timeout.hh>
#include <seastar/util/closeable.hh>
#include <seastar/core/abort_source.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/when_all.hh>
#include <seastar/net/api.hh>
#include <seastar/net/posix-stack.hh>
#include <stdexcept>

#include <fstream>
#include <optional>
#include <tuple>
#include <future>

using namespace seastar;

future<> handle_connection(connected_socket s) {
    auto in = s.input();
    auto out = s.output();
    return do_with(std::move(in), std::move(out), [](auto& in, auto& out) {
        return do_until([&in]() { return in.eof(); },
            [&in, &out] {
                return in.read().then([&out](auto buf) {
                    return out.write(std::move(buf)).then([&out]() { return out.close(); });
                });
            });
    });
}

future<> echo_server_loop() {
    return do_with(
        server_socket(listen(make_ipv4_address({1234}), listen_options{.reuse_address = true})), [](auto& listener) {
              // Connect asynchronously in background.
              (void)connect(make_ipv4_address({"127.0.0.1", 1234})).then([](connected_socket&& socket) {
                  socket.shutdown_output();
              });
              return listener.accept().then(
                  [](accept_result ar) {
                      connected_socket s = std::move(ar.connection);
                      return handle_connection(std::move(s));
                  }).then([l = std::move(listener)]() mutable { return l.abort_accept(); });
        });
}

class my_malloc_allocator : public std::pmr::memory_resource {
public:
    int allocs;
    int frees;
    void* do_allocate(std::size_t bytes, std::size_t alignment) override { allocs++; return malloc(bytes); }
    void do_deallocate(void *ptr, std::size_t bytes, std::size_t alignment) override { frees++; return free(ptr); }
    virtual bool do_is_equal(const std::pmr::memory_resource& __other) const noexcept override { abort(); }
};

my_malloc_allocator malloc_allocator;
std::pmr::polymorphic_allocator<char> allocator{&malloc_allocator};

SEASTAR_TEST_CASE(socket_allocation_test) {
    return echo_server_loop().finally([](){ engine().exit((malloc_allocator.allocs == malloc_allocator.frees) ? 0 : 1); });
}

SEASTAR_TEST_CASE(socket_skip_test) {
    return seastar::async([&] {
        listen_options lo;
        lo.reuse_address = true;
        server_socket ss = seastar::listen(ipv4_addr("127.0.0.1", 1234), lo);

        auto client = async([] {
            connected_socket socket = connect(ipv4_addr("127.0.0.1", 1234)).get();
            socket.output().write("abc").get();
            socket.shutdown_output();
        });

        accept_result accepted = ss.accept().get();
        input_stream<char> input = accepted.connection.input();
        // The client sends 3 bytes and shuts down, so skipping 16 bytes
        // hits EOF and must throw.
        BOOST_REQUIRE_THROW(input.skip(16).get(), std::runtime_error);
        client.get();
    });
}

SEASTAR_TEST_CASE(test_file_desc_fdinfo) {
    auto fd = file_desc::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    auto info = fd.fdinfo();
    BOOST_REQUIRE_EQUAL(info.substr(0, 8), "socket:[");
    return make_ready_future<>();
}

SEASTAR_TEST_CASE(socket_on_close_test) {
    listen_options lo;
    lo.reuse_address = true;
    server_socket ss = seastar::listen(ipv4_addr("127.0.0.1", 12345), lo);

    bool server_closed = false;
    bool client_notified = false;

    auto client = [&]() -> future<> {
        connected_socket cln = co_await connect(ipv4_addr("127.0.0.1", 12345));

        auto close_wait_fiber = cln.wait_input_shutdown().then([&] {
            BOOST_REQUIRE_EQUAL(server_closed, true);
            client_notified = true;
            fmt::print("Client: server closed\n");
        });

        auto out = cln.output();
        auto in = cln.input();

        while (!client_notified) {
            fmt::print("Client: -> message\n");
            co_await out.write("hello");
            co_await out.flush();
            co_await seastar::sleep(std::chrono::milliseconds(250));
            fmt::print("Client: <- message\n");
            auto buf = co_await in.read();
            if (!buf) {
                fmt::print("Client: server eof\n");
                break;
            }
            co_await seastar::sleep(std::chrono::milliseconds(250));
        }

        co_await out.close();
        co_await in.close();
        co_await std::move(close_wait_fiber);
    };

    auto server = [&]() -> future<> {
        accept_result acc = co_await ss.accept();
        auto out = acc.connection.output();
        auto in = acc.connection.input();

        for (int i = 0; i < 3; i++) {
            auto buf = co_await in.read();
            BOOST_REQUIRE_EQUAL(client_notified, false);
            co_await out.write(std::move(buf));
            co_await out.flush();
            fmt::print("Server: served\n");
        }

        server_closed = true;
        fmt::print("Server: closing\n");
        co_await out.close();
        co_await in.close();
    };

    co_await when_all(client(), server());
}

SEASTAR_TEST_CASE(socket_on_close_local_shutdown_test) {
    listen_options lo;
    lo.reuse_address = true;
    server_socket ss = seastar::listen(ipv4_addr("127.0.0.1", 12345), lo);

    bool server_closed = false;
    bool client_notified = false;

    auto client = [&]() -> future<> {
        connected_socket cln = co_await connect(ipv4_addr("127.0.0.1", 12345));

        auto close_wait_fiber = cln.wait_input_shutdown().then([&] {
            BOOST_REQUIRE_EQUAL(server_closed, false);
            client_notified = true;
            fmt::print("Client: socket closed\n");
        });

        auto out = cln.output();
        cln.shutdown_input();

        auto fin = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        do {
            co_await seastar::yield();
        } while (!client_notified && std::chrono::steady_clock::now() < fin);
        BOOST_REQUIRE_EQUAL(client_notified, true);

        co_await out.write("hello");
        co_await out.flush();
        co_await out.close();

        co_await std::move(close_wait_fiber);
    };

    auto server = [&]() -> future<> {
        accept_result acc = co_await ss.accept();
        auto in = acc.connection.input();
        auto buf = co_await in.read();
        server_closed = true;
        fmt::print("Server: closing\n");
        co_await in.close();
    };

    co_await when_all(client(), server());
}

// The test makes sure it's possible to abort connect()-ing a socket before
// it succeeds or fails. The way to abort the in-flight connection is to call
// shutdown() on the socket. The connect()'s future<> must resolve shortly
// after that with exception.
//
// The test currently fails on io_uring backend -- calling shutdown() doesn't
// make connect() future<> to resolve, instead it resolves after kernel times
// out the socket, which's not what test expects (see scylladb/seastar#2303)
SEASTAR_TEST_CASE(socket_connect_abort_test) {
    return seastar::async([&] {
        bool too_late = false;
        auto sk = make_socket();
        auto cf = sk.connect(ipv4_addr("192.0.2.1", 12345)).then([] (auto cs) {
            fmt::print("Connected\n");
            BOOST_REQUIRE(false);
        }).handle_exception([&too_late] (auto ex) {
            fmt::print("Cannot connect {}\n", seastar::formattable(ex));
            BOOST_REQUIRE(!too_late);
        });

        auto abort = sleep(std::chrono::milliseconds(500)).then([&sk] {
            fmt::print("Abort connect\n");
            sk.shutdown();
        });

        auto check = sleep(std::chrono::seconds(2)).then([&too_late] {
            fmt::print("Connection must have been aborted already\n");
            too_late = true;
        });

        when_all(std::move(cf), std::move(check), std::move(abort)).get();
    });
}

// Check that server_socket::accept() is abortable by server_socket::abort_accept()
SEASTAR_THREAD_TEST_CASE(socket_accept_abort_test) {
    ipv4_addr addr("127.0.0.1", 3174);
    server_socket ss = seastar::listen(addr, listen_options{ .reuse_address = true });
    bool too_late = false;
    auto f = async([&] {
        try {
            ss.accept().get();
            BOOST_FAIL("Accept didn't resolve into exception");
        } catch (const std::system_error& e) {
            BOOST_REQUIRE(!too_late);
            BOOST_REQUIRE_EQUAL(e.code(), std::error_code(ECONNABORTED, std::system_category()));
        }
    });

    auto abort = sleep(std::chrono::milliseconds(500)).then([&ss] {
        fmt::print("Abort accept\n");
        ss.abort_accept();
    });

    auto check = sleep(std::chrono::seconds(2)).then([&too_late] {
        fmt::print("Accept must have been aborted already\n");
        too_late = true;
    });

    when_all(std::move(f), std::move(abort), std::move(check)).get();
}

// Check that server_socket::abort_accept() and server_socket::local_address()
// are safe to call on an "empty" server_socket -- one that is either
// default-constructed or moved-from, and therefore has no underlying
// server_socket_impl. Previously these two methods dereferenced the
// underlying impl unconditionally and would crash.
SEASTAR_THREAD_TEST_CASE(socket_abort_accept_on_empty_test) {
    server_socket empty_ss;
    empty_ss.abort_accept();
    BOOST_CHECK(empty_ss.local_address().is_unspecified());

    server_socket live_ss = seastar::listen(ipv4_addr("127.0.0.1", 0), listen_options{ .reuse_address = true });
    server_socket moved_to = std::move(live_ss);
    // live_ss is now moved-from and empty; must still be safe to call.
    live_ss.abort_accept();
    BOOST_CHECK(live_ss.local_address().is_unspecified());
    moved_to.abort_accept();
}

SEASTAR_THREAD_TEST_CASE(udp_ipv4_wildcard_dst_test) {
    auto sc = make_bound_datagram_channel(ipv4_addr("0.0.0.0", 0));
    auto cc = make_bound_datagram_channel(ipv4_addr("127.0.0.1", 0));
    socket_address dst(ipv4_addr("127.0.0.1", sc.local_address().port()));

    cc.send(dst, "apa").get();
    auto pkt = sc.receive().get();

    BOOST_REQUIRE_EQUAL(pkt.get_dst(), dst);
    BOOST_REQUIRE_EQUAL(pkt.get_src(), cc.local_address());
    BOOST_REQUIRE_EQUAL(pkt.get_src().length(), sizeof(::sockaddr_in));

    cc.close();
    sc.close();
}

static bool dual_stack_sockets_available() {
    std::ifstream f("/proc/sys/net/ipv6/bindv6only");
    int bindv6only = 1;
    f >> bindv6only;
    return engine().net().supports_ipv6() && bindv6only == 0;
}

// the kernel reports both ends of an IPv4 datagram on a [::] channel as v4-mapped addresses
SEASTAR_THREAD_TEST_CASE(udp_dual_stack_v4_mapped_dst_test) {
    if (!dual_stack_sockets_available()) {
        return;
    }
    auto sc = make_bound_datagram_channel(ipv6_addr("::", 0));
    auto cc = make_bound_datagram_channel(ipv4_addr("127.0.0.1", 0));
    auto port = sc.local_address().port();

    cc.send(ipv4_addr("127.0.0.1", port), "apa").get();
    auto pkt = sc.receive().get();

    BOOST_REQUIRE_EQUAL(pkt.get_src().family(), AF_INET6);
    BOOST_REQUIRE_EQUAL(pkt.get_src().port(), cc.local_address().port());
    BOOST_REQUIRE_EQUAL(pkt.get_dst(), socket_address(ipv6_addr("::ffff:127.0.0.1", port)));

    cc.close();
    sc.close();
}

SEASTAR_THREAD_TEST_CASE(socket_bufsize) {

    // Test that setting the send and recv buffer sizes on the listening
    // socket is propagated to the socket returned by accept().

    auto buf_size = [](std::optional<int> snd_size, std::optional<int> rcv_size) {
        listen_options lo{
            .reuse_address = true,
            .lba = server_socket::load_balancing_algorithm::fixed,
            .so_sndbuf = snd_size,
            .so_rcvbuf = rcv_size
        };

        ipv4_addr addr("127.0.0.1", 1234);
        server_socket ss = seastar::listen(addr, lo);
        connected_socket client = connect(addr).get();
        connected_socket server = ss.accept().get().connection;

        auto sockopt = [&](int option) {
            int val{};
            int ret = server.get_sockopt(SOL_SOCKET, option, &val, sizeof(val));
            BOOST_REQUIRE_EQUAL(ret, 0);
            return val;
        };

        int send = sockopt(SO_SNDBUF);
        int recv = sockopt(SO_RCVBUF);

        ss.abort_accept();
        client.shutdown_output();
        server.shutdown_output();


        return std::make_tuple(send, recv);
    };

    constexpr int small_size = 8192, big_size = 128 * 1024;

    // we pass different sizes for send and recv to catch any copy/paste
    // style bugs
    auto [send_small, recv_small] = buf_size(small_size, small_size * 2);
    auto [send_big, recv_big] = buf_size(big_size, big_size * 2);

    // Setting socket buffer sizes isn't an exact science: the kernel does
    // some rounding, and also (currently) doubles the requested size and
    // also applies so limits. So as a basic check, assert simply that the
    // explicit small buffer ends up smaller than the explicit big buffer,
    // and that both results are at least as large as the requested amount.
    // The latter condition could plausibly fail if the OS clamped the size
    // at a small amount, but this is unlikely for the chosen buffer sizes.

    BOOST_CHECK_LT(send_small, send_big);
    BOOST_CHECK_LT(recv_small, recv_big);

    BOOST_CHECK_GE(send_small, small_size);
    BOOST_CHECK_GE(send_big, big_size);

    BOOST_CHECK_GE(recv_small, small_size * 2);
    BOOST_CHECK_GE(recv_big, big_size * 2);

    // not much to check here with "default" sizes, but let's at least call it
    // and check that we get a reasonable answer
    auto [send_default, recv_default] = buf_size({}, {});

    BOOST_CHECK_GE(send_default, 4096);
    BOOST_CHECK_GE(recv_default, 4096);

    // we don't really know the default socket size and it can vary by kernel
    // config, but 20 MB should be enough for everyone.
    BOOST_CHECK_LT(send_default, 20'000'000);
    BOOST_CHECK_LT(recv_default, 20'000'000);
}

// Replies to each accepted connection with the id of the shard that hosts it
// and the connection's remote port, and counts the connections it accepted.
// With hold, it keeps the connections open until held is cleared. Shards
// other than 0 start to accept after accept_delay.
struct shard_number_server {
    server_socket ss;
    bool hold;
    std::chrono::milliseconds accept_delay;
    std::vector<connected_socket> held;
    size_t accepted = 0;
    future<> runner;
    shard_number_server(socket_address addr, listen_options lo, bool hold = false, std::chrono::milliseconds accept_delay = {})
            : ss(seastar::listen(addr, lo))
            , hold(hold)
            , accept_delay(accept_delay)
            , runner(run()) {
    }
    future<> run() {
        try {
            if (this_shard_id() != 0 && accept_delay.count()) {
                // Shard 0 accepts at once: its accept() moves the connections. The other
                // shards accept late, so that their first connections wait in the queue.
                co_await seastar::sleep(accept_delay);
            }
            while (true) {
                auto [cs, _] = co_await ss.accept();
                ++accepted;
                auto out = cs.output();
                auto in = cs.input();
                char buf[8];
                write_be<unsigned>(buf, this_shard_id());
                write_be<unsigned>(buf + 4, cs.remote_address().port());
                if (hold) {
                    // Hold before the reply, so that a client that has the
                    // reply finds the connection held.
                    held.push_back(std::move(cs));
                }
                co_await out.write(buf, sizeof(buf));
                co_await out.close();
                co_await in.close();
            }
        } catch (...) {
            // expected on abort_accept
        }
    }
    future<> stop() {
        ss.abort_accept();
        return std::move(runner);
    }
};

static
void
test_load_balancing_algorithm_port(socket_address listen_addr, bool proxy_protocol) {
    auto& alien = engine().alien();
    listen_options lo;
    lo.reuse_address = true;
    lo.lba = server_socket::load_balancing_algorithm::port;
    lo.proxy_protocol = proxy_protocol;

    struct client_results {
        int attempts = 0;
        int bad_socket = 0;
        int bad_bind = 0;
        int bad_connect = 0;
        int bad_proxy_send = 0;
        int bad_recv = 0;
        int bad_shard = 0;
        int bad_remote_port = 0;
        int good = 0;
    };

    auto server = sharded<shard_number_server>();
    server.start(listen_addr, lo, false, std::chrono::milliseconds(50)).get();
    auto smp_count = this_smp_shard_count();
    promise<> client_done;
    auto client = std::async(std::launch::async, [&] {
        auto r = client_results{};
        for (unsigned i = 0; i < 100u; ++i) {
            ++r.attempts;
            uint16_t port = 20000 + i;
            unsigned expected_shard = port % smp_count;
            auto client_addr = listen_addr;
            switch (client_addr.family()) {
            case AF_INET: {
                auto& sa_in = client_addr.as_posix_sockaddr_in();
                sa_in.sin_port = htons(port);
                break;
            }
            case AF_INET6: {
                auto& sa_in6 = client_addr.as_posix_sockaddr_in6();
                sa_in6.sin6_port = htons(port);
                break;
            }
            default:
                std::abort();
            }
            int conn = ::socket(client_addr.family(), SOCK_STREAM, IPPROTO_TCP);
            if (conn == -1) {
                ++r.bad_socket;
                continue;
            }
            // set REUSEADDR to avoid port exhaustion in case of test failures
            int opt = 1;
            ::setsockopt(conn, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
            auto do_close = defer([conn] () noexcept { ::close(conn); });
            if (!proxy_protocol) {
                int bind_result = ::bind(conn, &client_addr.as_posix_sockaddr(), client_addr.length());
                if (bind_result == -1) {
                    ++r.bad_bind; // port may be busy;
                    continue;
                }
            }
            int conn_result = ::connect(conn, &listen_addr.as_posix_sockaddr(), listen_addr.length());
            if (conn_result == -1) {
                ++r.bad_connect;
                continue;
            }
            if (proxy_protocol) {
                // send a minimal PROXY protocol v2 header with correct source port
                char buf[16 + 36] = {
                    0x0d, 0x0a, 0x0d, 0x0a, 0x00, 0x0d, 0x0a, 0x51, 0x55, 0x49, 0x54, 0x0a, // signature
                };
                buf[12] = 0x21;                   // version and command (PROXY)
                buf[13] = (listen_addr.family() == AF_INET) ? 0x11 : 0x21; // family and protocol
                uint16_t xlen = (listen_addr.family() == AF_INET) ? 12 : 36;
                write_be<uint16_t>(buf + 14, xlen); // length
                if (listen_addr.family() == AF_INET) {
                    buf[16] = 127;
                    buf[17] = 0;
                    buf[18] = 0;
                    buf[19] = 1;
                    buf[20] = 127;
                    buf[21] = 0;
                    buf[22] = 0;
                    buf[23] = 1;
                    write_be<uint16_t>(buf + 24, client_addr.port()); // source port
                    write_be<uint16_t>(buf + 26, listen_addr.port()); // destination port
                } else {
                    buf[31] = 1;
                    buf[47] = 1;
                    write_be<uint16_t>(buf + 48, client_addr.port()); // source port
                    write_be<uint16_t>(buf + 50, listen_addr.port()); // destination port
                }
                auto proxy_send_result = ::send(conn, buf, 16 + xlen, 0);
                if (proxy_send_result != 16 + xlen) {
                    ++r.bad_proxy_send;
                    continue;
                }
            }
            char buf[8];
            auto recv_result = ::recv(conn, buf, sizeof(buf), MSG_WAITALL);
            if (recv_result != 8) {
                ++r.bad_recv;
                continue;
            }
            auto actual_shard = read_be<unsigned>(buf);
            if (actual_shard != expected_shard) {
                ++r.bad_shard;
                continue;
            }
            if (read_be<unsigned>(buf + 4) != client_addr.port()) {
                ++r.bad_remote_port;
                continue;
            }
            ++r.good;
        }
        alien::submit_to(alien, 0, [&] { client_done.set_value(); return make_ready_future<>(); });
        return r;
    });
    client_done.get_future().get();
    server.stop().get();
    auto results = client.get();
    BOOST_REQUIRE_EQUAL(results.attempts, 100);
    BOOST_REQUIRE_EQUAL(results.bad_socket, 0);
    // We can have bad binds due to other connection using the ports.
    // 20 should be plenty of margin.
    BOOST_REQUIRE_LE(results.bad_bind, 20);
    BOOST_REQUIRE_EQUAL(results.bad_connect, 0);
    BOOST_REQUIRE_EQUAL(results.bad_proxy_send, 0);
    BOOST_REQUIRE_EQUAL(results.bad_recv, 0);
    BOOST_REQUIRE_EQUAL(results.bad_shard, 0);
    BOOST_REQUIRE_EQUAL(results.bad_remote_port, 0);
}

// A random loopback address for a test that listens with SO_REUSEPORT on a
// fixed port: a parallel run in another process on the same address would
// silently share the connections.
static socket_address random_loopback(uint16_t port) {
    return ipv4_addr(std::uniform_int_distribution<uint32_t>(0x7f000002, 0x7ffffffe)(testing::local_random_engine), port);
}

SEASTAR_THREAD_TEST_CASE(load_balancing_algorithm_port_ipv4_test) {
    test_load_balancing_algorithm_port(random_loopback(11001), false);
}

SEASTAR_THREAD_TEST_CASE(load_balancing_algorithm_port_ipv6_test) {
    test_load_balancing_algorithm_port(ipv6_addr("::1", 11001), false);
}

SEASTAR_THREAD_TEST_CASE(load_balancing_algorithm_port_ipv4_proxy_test) {
    test_load_balancing_algorithm_port(ipv4_addr("127.0.0.1", 11001), true);
}

SEASTAR_THREAD_TEST_CASE(load_balancing_algorithm_port_ipv6_proxy_test) {
    test_load_balancing_algorithm_port(ipv6_addr("::1", 11001), true);
}

// The total of every shard's hosted-connection count for the endpoint.
static long hosted_total(socket_address addr) {
    auto ep = net::conntrack::endpoint(int(transport::TCP), addr);
    long total = 0;
    for (unsigned cpu = 0; cpu < this_smp_shard_count(); ++cpu) {
        total += smp::submit_to(cpu, [ep] { return net::conntrack::hosted_count(ep); }).get();
    }
    return total;
}

// Opens connections to addr, at most concurrency at a time; each one reads the
// server's reply and closes.
static future<> churn(socket_address addr, unsigned connections, unsigned concurrency) {
    auto ids = std::views::iota(0u, connections);
    co_await max_concurrent_for_each(ids, concurrency, [addr] (unsigned) {
        return connect(addr).then([] (connected_socket cs) {
            return do_with(std::move(cs), [] (connected_socket& cs) {
                return do_with(cs.input(), [] (input_stream<char>& in) {
                    return in.read_exactly(8).discard_result().finally([&in] { return in.close(); });
                });
            });
        });
    });
}

// Check that connection-distribution balancing spreads connections evenly
// across the shards, whichever accept path serves the endpoint (the
// single-listener conntrack balancer or per-shard SO_REUSEPORT listeners;
// churn: the per-shard listeners only).
//
// held: each connection is acknowledged by its hosting shard before the next
// one opens, and all stay open, so the balancer sees the load it balances.
// The fill repeats from zero for several rounds, and the test bounds the mean
// spread: a listener decides on its last snapshot plus its own placements, so
// it trails the other listeners and one round can spread wide, while the raw
// kernel hash spreads several times wider on average. The two sizes cover
// both regions of the view's refresh probability.
// churn: every shard opens short-lived connections, 10 at a time - the
// pattern of #3514. With one listener on shard 0, shard 0 gets every
// connection. With a listener per shard, a shard gets about 0.7 to 1.4 of
// its share (--smp 4): least-connections gives more to the shards whose
// connections close sooner.
SEASTAR_THREAD_TEST_CASE(load_balancing_connection_distribution_spread_test) {
    const auto shards = this_smp_shard_count();
    const auto listen_addr = random_loopback(11002);
    const auto lo = listen_options{ .reuse_address = true };

    // held
    {
        auto server = sharded<shard_number_server>();
        server.start(listen_addr, lo, true).get();
        auto stop_server = deferred_stop(server);
        constexpr size_t rounds = 5;
        for (size_t per_shard : {size_t(net::sampled_conntrack::refresh_weight), 4 * size_t(net::sampled_conntrack::refresh_weight)}) {
            size_t spread_sum = 0;
            for (size_t round = 0; round < rounds; ++round) {
                std::vector<connected_socket> conns;
                std::vector<size_t> held(shards);
                for (size_t i = 0; i < per_shard * shards; ++i) {
                    auto cs = connect(listen_addr).get();
                    auto ack = cs.input().read_exactly(8).get();
                    BOOST_REQUIRE_EQUAL(ack.size(), 8u);
                    ++held.at(read_be<unsigned>(ack.get()));
                    conns.push_back(std::move(cs));
                }
                auto [min_held, max_held] = std::ranges::minmax(held);
                BOOST_TEST_MESSAGE(fmt::format("held {} per shard, round {}: {}", per_shard, round, held));
                spread_sum += max_held - min_held;

                conns.clear();
                server.invoke_on_all([] (shard_number_server& s) { s.held.clear(); }).get();
                // Every connection is closed: a leaked hosted count would skew
                // every later view of the endpoint.
                BOOST_REQUIRE_EQUAL(hosted_total(listen_addr), 0);
            }
            BOOST_REQUIRE_LE(spread_sum, rounds * std::max<size_t>(3, shards));
        }
    }

    // churn
    if (!engine().posix_reuseport_available()) {
        // One listener on shard 0 keeps the lag of #3514: shard 0 gets all.
        BOOST_TEST_MESSAGE("SO_REUSEPORT is not available, skipping churn");
        return;
    }
    auto server = sharded<shard_number_server>();
    server.start(listen_addr, lo).get();
    auto stop_server = deferred_stop(server);
    constexpr unsigned churn_per_shard = 500;
    smp::invoke_on_all([listen_addr] { return churn(listen_addr, churn_per_shard, 10); }).get();
    auto accepted = server.map([] (shard_number_server& s) { return s.accepted; }).get();
    BOOST_TEST_MESSAGE(fmt::format("churn: {}", accepted));
    auto [min_accepted, max_accepted] = std::ranges::minmax(accepted);
    BOOST_REQUIRE_GE(min_accepted, 0.5 * churn_per_shard);
    BOOST_REQUIRE_LE(max_accepted, 1.5 * churn_per_shard);
    stop_server.stop_now();
    BOOST_REQUIRE_EQUAL(hosted_total(listen_addr), 0);
}

// Check the accept() contract of a reuseport listener: a connection that
// arrives before accept() is served by it; abort_accept() fails the pending
// accept() and every later one with ECONNABORTED, and the snapshots then
// mark the shard as one that takes no connections; destruction without
// abort_accept() fails the pending accept() and refuses new connections,
// instead of leaving the armed kernel accept serving the endpoint.
SEASTAR_THREAD_TEST_CASE(reuseport_listener_lifecycle_test) {
    if (!engine().posix_reuseport_available()) {
        BOOST_TEST_MESSAGE("SO_REUSEPORT is not available, skipping");
        return;
    }
    const auto listen_addr = random_loopback(11004);
    const auto lo = listen_options{ .reuse_address = true };
    auto aborted = [] (const std::system_error& e) {
        return e.code() == std::error_code(ECONNABORTED, std::system_category());
    };
    {
        server_socket ss = seastar::listen(listen_addr, lo);
        auto cs = connect(listen_addr).get();
        auto ar = ss.accept().get();
        BOOST_REQUIRE_EQUAL(ar.remote_address.port(), cs.local_address().port());
        ss.abort_accept();
    }
    {
        server_socket ss = seastar::listen(listen_addr, lo);
        auto ep = net::conntrack::endpoint(int(transport::TCP), listen_addr);
        BOOST_REQUIRE_NE(net::conntrack::collect_hosted(ep).get()[this_shard_id()], net::conntrack::not_listening);
        auto af = ss.accept();
        ss.abort_accept();
        BOOST_REQUIRE_EXCEPTION(af.get(), std::system_error, aborted);
        BOOST_REQUIRE_EXCEPTION(ss.accept().get(), std::system_error, aborted);
        BOOST_REQUIRE_EQUAL(net::conntrack::collect_hosted(ep).get()[this_shard_id()], net::conntrack::not_listening);
    }
    {
        std::optional<server_socket> ss = seastar::listen(listen_addr, lo);
        // A full accept first, so the listener has armed its kernel accept.
        auto af = ss->accept();
        auto cs = connect(listen_addr).get();
        auto ar = af.get();
        auto af2 = ss->accept();
        ss.reset();
        BOOST_REQUIRE_EXCEPTION(af2.get(), std::system_error, aborted);
        BOOST_REQUIRE_THROW(connect(listen_addr).get(), std::system_error);
    }
}

// Check that a server that listens on one shard only, with the default
// balancing, receives every connection: no connection may go to a shard
// without a listener, where it would be closed.
SEASTAR_THREAD_TEST_CASE(single_shard_listener_test) {
    const auto listen_addr = random_loopback(11006);
    server_socket ss = seastar::listen(listen_addr, listen_options{ .reuse_address = true });
    for (int i = 0; i < 20; ++i) {
        auto af = ss.accept();
        auto cs = connect(listen_addr).get();
        with_timeout(lowres_clock::now() + std::chrono::seconds(5), std::move(af)).get();
    }
    ss.abort_accept();
}

// Check that ephemeral-port (port 0) listens keep the classic single-listener
// semantics: two such listens on one shard must succeed and bind two distinct
// ports. The reuseport path must not engage for port 0 - each shard would
// bind a different port, so the kernel could not spread connections anyway.
SEASTAR_THREAD_TEST_CASE(ephemeral_port_listen_test) {
    auto lo = listen_options{ .reuse_address = true };
    server_socket ss1 = seastar::listen(ipv4_addr("127.0.0.1", 0), lo);
    server_socket ss2 = seastar::listen(ipv4_addr("127.0.0.1", 0), lo);
    auto a1 = ss1.local_address();
    auto a2 = ss2.local_address();
    BOOST_REQUIRE_NE(a1.port(), 0);
    BOOST_REQUIRE_NE(a2.port(), 0);
    BOOST_REQUIRE_NE(a1.port(), a2.port());
    // And the sockets accept connections. The listener exists on this shard
    // only, so this relies on the single-listener balancer resolving the
    // all-idle tie to shard 0, the shard the test runs on. The test cannot pin
    // the listener: a fixed-cpu listen would not reach the port-0 path.
    auto af = ss1.accept();
    auto cs = connect(a1).get();
    auto ar = af.get();
    ss1.abort_accept();
    ss2.abort_accept();
}

// Check listen_options::reuse_port, for connection-distribution and port
// balancing. With it (the default), another socket of the same user can bind
// the listener's address with SO_REUSEPORT, and so take a share of its
// connections, as a second process would. Without it, that bind fails with
// EADDRINUSE.
SEASTAR_THREAD_TEST_CASE(reuse_port_listen_option_test) {
    if (!engine().posix_reuseport_available()) {
        BOOST_TEST_MESSAGE("SO_REUSEPORT is not available, skipping");
        return;
    }
    auto addr = random_loopback(11005);
    for (auto lba : {server_socket::load_balancing_algorithm::connection_distribution, server_socket::load_balancing_algorithm::port}) {
        for (bool reuse_port : {true, false}) {
            server_socket ss = seastar::listen(addr, listen_options{ .reuse_address = true, .lba = lba, .reuse_port = reuse_port });
            auto other = file_desc::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
            other.setsockopt(SOL_SOCKET, SO_REUSEPORT, 1);
            if (reuse_port) {
                other.bind(addr.u.sa, addr.length());
            } else {
                BOOST_REQUIRE_EXCEPTION(other.bind(addr.u.sa, addr.length()), std::system_error, [] (const std::system_error& e) {
                    return e.code() == std::error_code(EADDRINUSE, std::system_category());
                });
            }
            ss.abort_accept();
        }
    }
}

SEASTAR_THREAD_TEST_CASE(inet_local_remote_address_sanity) {
    auto addr = make_ipv4_address(11003);
    auto ls = listen(addr);
    auto ar_f = ls.accept();

    std::optional<connected_socket> cs = connect(addr).get();
    auto ar = ar_f.get();
    auto ss = std::move(ar.connection);

    BOOST_CHECK_EQUAL(cs->local_address(), ss.remote_address());
    BOOST_CHECK_EQUAL(cs->remote_address(), ss.local_address());

    // Now disconnect the server socket on the kernel level
    // For that -- write some data into client, then close the client. When
    // it happens, kernel forces connection reset and server socket will
    // get into unconnected state
    auto sout = ss.output();
    sout.write("data").get();
    sout.flush().get();
    sout.close().get();
    // Sockets are batch-flushed, so we need to give it a time to get
    // flush-polled and also let kernel transfer the data into client
    // socket
    seastar::sleep(std::chrono::milliseconds(500)).get();
    // Close the socket. Closing in/out streams won't work, it will shutdown
    // the socket and shutting down doesn't send RST-s
    cs.reset();

    ss.wait_input_shutdown().get();
    BOOST_CHECK(ss.remote_address().is_unspecified());
}
