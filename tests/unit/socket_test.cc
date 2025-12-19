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
#include <seastar/core/abort_source.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/when_all.hh>
#include <seastar/net/api.hh>
#include <seastar/net/posix-stack.hh>

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

        abort_source as;
        auto client = async([&as] {
            connected_socket socket = connect(ipv4_addr("127.0.0.1", 1234)).get();
            socket.output().write("abc").get();
            socket.shutdown_output();
            try {
                sleep_abortable(std::chrono::seconds(10), as).get();
            } catch (const sleep_aborted&) {
                // expected
                return;
            }
            SEASTAR_ASSERT(!"Skipping data from socket is likely stuck");
        });

        accept_result accepted = ss.accept().get();
        input_stream<char> input = accepted.connection.input();
        input.skip(16).get();
        as.request_abort();
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
    return seastar::async([&] {
        listen_options lo;
        lo.reuse_address = true;
        server_socket ss = seastar::listen(ipv4_addr("127.0.0.1", 12345), lo);

        bool server_closed = false;
        bool client_notified = false;

        auto client = seastar::async([&] {
            connected_socket cln = connect(ipv4_addr("127.0.0.1", 12345)).get();

            auto close_wait_fiber = cln.wait_input_shutdown().then([&] {
                BOOST_REQUIRE_EQUAL(server_closed, true);
                client_notified = true;
                fmt::print("Client: server closed\n");
            });

            auto out = cln.output();
            auto in = cln.input();

            while (!client_notified) {
                fmt::print("Client: -> message\n");
                out.write("hello").get();
                out.flush().get();
                seastar::sleep(std::chrono::milliseconds(250)).get();
                fmt::print("Client: <- message\n");
                auto buf = in.read().get();
                if (!buf) {
                    fmt::print("Client: server eof\n");
                    break;
                }
                seastar::sleep(std::chrono::milliseconds(250)).get();
            }

            out.close().get();
            in.close().get();
            close_wait_fiber.get();
        });

        auto server = seastar::async([&] {
            accept_result acc = ss.accept().get();
            auto out = acc.connection.output();
            auto in = acc.connection.input();

            for (int i = 0; i < 3; i++) {
                auto buf = in.read().get();
                BOOST_REQUIRE_EQUAL(client_notified, false);
                out.write(std::move(buf)).get();
                out.flush().get();
                fmt::print("Server: served\n");
            }

            server_closed = true;
            fmt::print("Server: closing\n");
            out.close().get();
            in.close().get();
        });

        when_all(std::move(client), std::move(server)).discard_result().get();
    });
}

SEASTAR_TEST_CASE(socket_on_close_local_shutdown_test) {
    return seastar::async([&] {
        listen_options lo;
        lo.reuse_address = true;
        server_socket ss = seastar::listen(ipv4_addr("127.0.0.1", 12345), lo);

        bool server_closed = false;
        bool client_notified = false;

        auto client = seastar::async([&] {
            connected_socket cln = connect(ipv4_addr("127.0.0.1", 12345)).get();

            auto close_wait_fiber = cln.wait_input_shutdown().then([&] {
                BOOST_REQUIRE_EQUAL(server_closed, false);
                client_notified = true;
                fmt::print("Client: socket closed\n");
            });

            auto out = cln.output();
            cln.shutdown_input();

            auto fin = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            do {
                seastar::yield().get();
            } while (!client_notified && std::chrono::steady_clock::now() < fin);
            BOOST_REQUIRE_EQUAL(client_notified, true);

            out.write("hello").get();
            out.flush().get();
            out.close().get();

            close_wait_fiber.get();
        });

        auto server = seastar::async([&] {
            accept_result acc = ss.accept().get();
            auto in = acc.connection.input();
            auto buf = in.read().get();
            server_closed = true;
            fmt::print("Server: closing\n");
            in.close().get();
        });

        when_all(std::move(client), std::move(server)).discard_result().get();
    });
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
            fmt::print("Cannot connect {}\n", ex);
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
        int good = 0;
    };

    struct shard_number_server {
        server_socket ss;
        future<> runner;
        shard_number_server(socket_address addr, listen_options lo)
                : ss(seastar::listen(addr, lo))
                , runner(run()) {
        }
        future<> run() {
            try {
                while (true) {
                    auto [cs, _] = co_await ss.accept();
                    auto out = cs.output();
                    auto in = cs.input();
                    unsigned shard_id = this_shard_id();
                    char buf[4];
                    write_be<unsigned>(buf, shard_id);
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
    auto server = sharded<shard_number_server>();
    server.start(listen_addr, lo).get();
    auto smp_count = smp::count;
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
            char buf[4];
            auto recv_result = ::recv(conn, buf, sizeof(buf), 0);
            if (recv_result != 4) {
                ++r.bad_recv;
                continue;
            }
            auto actual_shard = read_be<unsigned>(buf);
            if (actual_shard != expected_shard) {
                ++r.bad_shard;
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
}

SEASTAR_THREAD_TEST_CASE(load_balancing_algorithm_port_ipv4_test) {
    test_load_balancing_algorithm_port(ipv4_addr("127.0.0.1", 11001), false);
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
