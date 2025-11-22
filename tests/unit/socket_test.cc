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
#include <seastar/core/timer.hh>
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

// Comprehensive tests for proxy protocol v2 implementation

// Helper function to create a valid proxy protocol v2 header
static std::vector<char> make_proxy_v2_header(
    bool valid_signature = true,
    uint8_t version_cmd = 0x21,  // v2 PROXY
    uint8_t family_proto = 0x11, // IPv4 TCP
    socket_address src_addr = ipv4_addr("192.168.1.100", 5000),
    socket_address dst_addr = ipv4_addr("10.0.0.1", 8080),
    std::vector<char> tlvs = {}) {

    std::vector<char> header;

    // Signature (12 bytes)
    if (valid_signature) {
        const char sig[12] = {0x0d, 0x0a, 0x0d, 0x0a, 0x00, 0x0d, 0x0a, 0x51, 0x55, 0x49, 0x54, 0x0a};
        header.insert(header.end(), sig, sig + 12);
    } else {
        const char bad_sig[12] = {0x0d, 0x0a, 0x0d, 0x0a, 0x00, 0x0d, 0x0a, 0x51, 0x55, 0x49, 0x54, 0x0b};
        header.insert(header.end(), bad_sig, bad_sig + 12);
    }

    // Version and command
    header.push_back(version_cmd);

    // Family and protocol
    header.push_back(family_proto);

    // Length (will be filled in later)
    size_t len_pos = header.size();
    header.push_back(0);
    header.push_back(0);

    // Address information
    uint16_t addr_len = 0;
    if ((version_cmd & 0x0F) == 0x01) { // PROXY command
        if (family_proto == 0x11) { // IPv4 TCP
            addr_len = 12;
            auto src_in = src_addr.as_posix_sockaddr_in();
            auto dst_in = dst_addr.as_posix_sockaddr_in();

            // Source address
            header.push_back((src_in.sin_addr.s_addr >> 0) & 0xFF);
            header.push_back((src_in.sin_addr.s_addr >> 8) & 0xFF);
            header.push_back((src_in.sin_addr.s_addr >> 16) & 0xFF);
            header.push_back((src_in.sin_addr.s_addr >> 24) & 0xFF);

            // Destination address
            header.push_back((dst_in.sin_addr.s_addr >> 0) & 0xFF);
            header.push_back((dst_in.sin_addr.s_addr >> 8) & 0xFF);
            header.push_back((dst_in.sin_addr.s_addr >> 16) & 0xFF);
            header.push_back((dst_in.sin_addr.s_addr >> 24) & 0xFF);

            // Source port
            uint16_t src_port = ntohs(src_in.sin_port);
            header.push_back((src_port >> 8) & 0xFF);
            header.push_back((src_port >> 0) & 0xFF);

            // Destination port
            uint16_t dst_port = ntohs(dst_in.sin_port);
            header.push_back((dst_port >> 8) & 0xFF);
            header.push_back((dst_port >> 0) & 0xFF);
        } else if (family_proto == 0x21) { // IPv6 TCP
            addr_len = 36;
            auto src_in6 = src_addr.as_posix_sockaddr_in6();
            auto dst_in6 = dst_addr.as_posix_sockaddr_in6();

            // Source address (16 bytes)
            header.insert(header.end(),
                reinterpret_cast<const char*>(&src_in6.sin6_addr),
                reinterpret_cast<const char*>(&src_in6.sin6_addr) + 16);

            // Destination address (16 bytes)
            header.insert(header.end(),
                reinterpret_cast<const char*>(&dst_in6.sin6_addr),
                reinterpret_cast<const char*>(&dst_in6.sin6_addr) + 16);

            // Source port
            uint16_t src_port = ntohs(src_in6.sin6_port);
            header.push_back((src_port >> 8) & 0xFF);
            header.push_back((src_port >> 0) & 0xFF);

            // Destination port
            uint16_t dst_port = ntohs(dst_in6.sin6_port);
            header.push_back((dst_port >> 8) & 0xFF);
            header.push_back((dst_port >> 0) & 0xFF);
        }
    } else if ((version_cmd & 0x0F) == 0x00) { // LOCAL command
        addr_len = 0;  // No address data for LOCAL
    }

    // Add TLVs
    header.insert(header.end(), tlvs.begin(), tlvs.end());
    addr_len += tlvs.size();

    // Fill in length
    header[len_pos] = (addr_len >> 8) & 0xFF;
    header[len_pos + 1] = addr_len & 0xFF;

    return header;
}

// Helper function for negative tests - expects connection to be rejected
static void test_proxy_header_negative(
    socket_address listen_addr,
    std::vector<char> header) {

    listen_options lo;
    lo.reuse_address = true;
    lo.proxy_protocol = true;

    server_socket ss = seastar::listen(listen_addr, lo);

    bool got_connection = false;

    auto server = seastar::async([&ss, &got_connection] {
        using namespace std::chrono_literals;
        // Set a timer to abort accept after 100ms
        timer<> abort_timer([&ss] {
            ss.abort_accept();
        });
        abort_timer.arm(timer<>::clock::now() + 100ms);

        try {
            auto ar = ss.accept().get();
            got_connection = true;
            ar.connection.shutdown_output();
        } catch (...) {
            // Expected - accept was aborted or connection rejected
        }
    });

    auto client = seastar::async([&listen_addr, header = std::move(header)] {
        try {
            auto s = connect(listen_addr).get();
            auto out = s.output();
            out.write(header.data(), header.size()).get();
            out.flush().get();
            out.close().get();
        } catch (...) {
            // Expected - server may close connection during write/flush/close
        }
    });

    when_all(std::move(server), std::move(client)).discard_result().get();
    ss.abort_accept();

    BOOST_REQUIRE(!got_connection);
}

SEASTAR_THREAD_TEST_CASE(proxy_protocol_v2_bad_signature) {
    // Invalid signature should cause connection to be dropped
    auto header = make_proxy_v2_header(false);  // bad signature
    test_proxy_header_negative(ipv4_addr("127.0.0.1", 12001), std::move(header));
}

SEASTAR_THREAD_TEST_CASE(proxy_protocol_v2_small_packet_incomplete_header) {
    listen_options lo;
    lo.reuse_address = true;
    lo.proxy_protocol = true;

    auto listen_addr = ipv4_addr("127.0.0.1", 12002);
    server_socket ss = seastar::listen(listen_addr, lo);

    bool got_connection = false;

    auto server = seastar::async([&ss, &got_connection] {
        using namespace std::chrono_literals;
        // Set a timer to abort accept after 100ms
        timer<> abort_timer([&ss] {
            ss.abort_accept();
        });
        abort_timer.arm(timer<>::clock::now() + 100ms);

        try {
            auto ar = ss.accept().get();
            got_connection = true;
            ar.connection.shutdown_output();
        } catch (...) {
            // Expected - abort_accept throws
        }
    });

    // Send only 10 bytes (header needs 16)
    auto client = seastar::async([&listen_addr] {
        try {
            auto s = connect(listen_addr).get();
            auto out = s.output();
            char partial[10] = {0x0d, 0x0a, 0x0d, 0x0a, 0x00, 0x0d, 0x0a, 0x51, 0x55, 0x49};
            out.write(partial, sizeof(partial)).get();
            out.flush().get();
            out.close().get();
        } catch (...) {
            // ignore
        }
    });

    when_all(std::move(server), std::move(client)).discard_result().get();
    ss.abort_accept();

    BOOST_REQUIRE(!got_connection);
}

SEASTAR_THREAD_TEST_CASE(proxy_protocol_v2_small_packet_incomplete_addresses) {
    listen_options lo;
    lo.reuse_address = true;
    lo.proxy_protocol = true;

    auto listen_addr = ipv4_addr("127.0.0.1", 12003);
    server_socket ss = seastar::listen(listen_addr, lo);

    bool got_connection = false;

    auto server = seastar::async([&ss, &got_connection] {
        using namespace std::chrono_literals;
        // Set a timer to abort accept after 100ms
        timer<> abort_timer([&ss] {
            ss.abort_accept();
        });
        abort_timer.arm(timer<>::clock::now() + 100ms);

        try {
            auto ar = ss.accept().get();
            got_connection = true;
            ar.connection.shutdown_output();
        } catch (...) {
            // Expected - abort_accept throws
        }
    });

    // Send header claiming 12 bytes of addresses but only send 6
    auto client = seastar::async([&listen_addr] {
        try {
            auto s = connect(listen_addr).get();
            auto out = s.output();
            unsigned char buf[22];
            const unsigned char sig[12] = {0x0d, 0x0a, 0x0d, 0x0a, 0x00, 0x0d, 0x0a, 0x51, 0x55, 0x49, 0x54, 0x0a};
            std::memcpy(buf, sig, 12);
            buf[12] = 0x21; // v2 PROXY
            buf[13] = 0x11; // IPv4 TCP
            buf[14] = 0x00; // length high byte
            buf[15] = 0x0C; // length low byte (12 bytes)
            // Only send 6 bytes of address data
            buf[16] = 192;
            buf[17] = 168;
            buf[18] = 1;
            buf[19] = 100;
            buf[20] = 10;
            buf[21] = 0;
            out.write(reinterpret_cast<char*>(buf), sizeof(buf)).get();
            out.flush().get();
            out.close().get();
        } catch (...) {
            // ignore
        }
    });

    when_all(std::move(server), std::move(client)).discard_result().get();
    ss.abort_accept();

    BOOST_REQUIRE(!got_connection);
}

SEASTAR_THREAD_TEST_CASE(proxy_protocol_v2_local_mode) {
    listen_options lo;
    lo.reuse_address = true;
    lo.proxy_protocol = true;

    auto listen_addr = ipv4_addr("127.0.0.1", 12004);
    server_socket ss = seastar::listen(listen_addr, lo);

    socket_address actual_remote;
    socket_address actual_local;
    socket_address client_actual_local;

    auto server = seastar::async([&ss, &actual_remote, &actual_local] {
        auto ar = ss.accept().get();
        actual_remote = ar.connection.remote_address();
        actual_local = ar.connection.local_address();
        ar.connection.shutdown_output();
    });

    // Send LOCAL command with UNSPEC family
    auto header = make_proxy_v2_header(true, 0x20, 0x00);  // v2 LOCAL, UNSPEC
    auto client = seastar::async([&listen_addr, &client_actual_local, header = std::move(header)] {
        auto s = connect(listen_addr).get();
        client_actual_local = s.local_address();
        auto out = s.output();
        out.write(header.data(), header.size()).get();
        out.flush().get();
        out.close().get();
    });

    when_all(std::move(server), std::move(client)).discard_result().get();
    ss.abort_accept();

    // With LOCAL mode, addresses should be real connection addresses, not from header
    // We can't check exact match since the client port is ephemeral, but we can verify
    // that the addresses are not the fake proxy addresses and the IPs match
    BOOST_REQUIRE_EQUAL(actual_local, socket_address(listen_addr));
    BOOST_REQUIRE_EQUAL(actual_remote, client_actual_local);
}

SEASTAR_THREAD_TEST_CASE(proxy_protocol_v2_local_mode_wrong_family) {
    // LOCAL command must use UNSPEC family (0x00), not other families
    auto header = make_proxy_v2_header(true, 0x20, 0x11);  // v2 LOCAL, but IPv4 TCP (invalid)
    test_proxy_header_negative(ipv4_addr("127.0.0.1", 12005), std::move(header));
}

SEASTAR_THREAD_TEST_CASE(proxy_protocol_v2_ipv4_addresses) {
    listen_options lo;
    lo.reuse_address = true;
    lo.proxy_protocol = true;

    auto listen_addr = ipv4_addr("127.0.0.1", 12006);
    server_socket ss = seastar::listen(listen_addr, lo);

    auto expected_remote = ipv4_addr("192.168.1.100", 5000);
    auto expected_local = ipv4_addr("10.0.0.1", 8080);

    socket_address actual_remote;
    socket_address actual_local;

    auto server = seastar::async([&ss, &actual_remote, &actual_local] {
        auto ar = ss.accept().get();
        actual_remote = ar.connection.remote_address();
        actual_local = ar.connection.local_address();
        ar.connection.shutdown_output();
    });

    auto header = make_proxy_v2_header(true, 0x21, 0x11, expected_remote, expected_local);
    auto client = seastar::async([&listen_addr, header = std::move(header)] {
        auto s = connect(listen_addr).get();
        auto out = s.output();
        out.write(header.data(), header.size()).get();
        out.flush().get();
        out.close().get();
    });

    when_all(std::move(server), std::move(client)).discard_result().get();
    ss.abort_accept();

    // Verify addresses match what was sent in the header
    BOOST_REQUIRE_EQUAL(actual_remote, socket_address(expected_remote));
    BOOST_REQUIRE_EQUAL(actual_local, socket_address(expected_local));
}

SEASTAR_THREAD_TEST_CASE(proxy_protocol_v2_ipv6_addresses) {
    listen_options lo;
    lo.reuse_address = true;
    lo.proxy_protocol = true;

    auto listen_addr = ipv6_addr("::1", 12007);
    server_socket ss = seastar::listen(listen_addr, lo);

    auto expected_remote = ipv6_addr("2001:db8::1", 5000);
    auto expected_local = ipv6_addr("2001:db8::2", 8080);

    socket_address actual_remote;
    socket_address actual_local;

    auto server = seastar::async([&ss, &actual_remote, &actual_local] {
        auto ar = ss.accept().get();
        actual_remote = ar.connection.remote_address();
        actual_local = ar.connection.local_address();
        ar.connection.shutdown_output();
    });

    auto header = make_proxy_v2_header(true, 0x21, 0x21, expected_remote, expected_local);
    auto client = seastar::async([&listen_addr, header = std::move(header)] {
        auto s = connect(listen_addr).get();
        auto out = s.output();
        out.write(header.data(), header.size()).get();
        out.flush().get();
        out.close().get();
    });

    when_all(std::move(server), std::move(client)).discard_result().get();
    ss.abort_accept();

    // Verify addresses match what was sent in the header
    BOOST_REQUIRE_EQUAL(actual_remote, socket_address(expected_remote));
    BOOST_REQUIRE_EQUAL(actual_local, socket_address(expected_local));
}

SEASTAR_THREAD_TEST_CASE(proxy_protocol_v2_invalid_version) {
    // Only version 2 (0x2X) is supported
    auto header = make_proxy_v2_header(true, 0x11);  // version 1 (invalid for v2 format)
    test_proxy_header_negative(ipv4_addr("127.0.0.1", 12008), std::move(header));
}

SEASTAR_THREAD_TEST_CASE(proxy_protocol_v2_invalid_command) {
    // Only LOCAL (0x0) and PROXY (0x1) commands are valid
    auto header = make_proxy_v2_header(true, 0x22);  // v2 with invalid command (0x2)
    test_proxy_header_negative(ipv4_addr("127.0.0.1", 12009), std::move(header));
}

SEASTAR_THREAD_TEST_CASE(proxy_protocol_v2_unsupported_family) {
    // Only AF_INET (0x1) and AF_INET6 (0x2) are supported for PROXY command
    auto header = make_proxy_v2_header(true, 0x21, 0x31);  // v2 PROXY, AF_UNIX (not supported)
    test_proxy_header_negative(ipv4_addr("127.0.0.1", 12010), std::move(header));
}

SEASTAR_THREAD_TEST_CASE(proxy_protocol_v2_unsupported_protocol) {
    // Only STREAM (0x1) is supported
    auto header = make_proxy_v2_header(true, 0x21, 0x12);  // v2 PROXY, IPv4 DGRAM (not supported)
    test_proxy_header_negative(ipv4_addr("127.0.0.1", 12011), std::move(header));
}

SEASTAR_THREAD_TEST_CASE(proxy_protocol_v2_length_too_short_ipv4) {
    listen_options lo;
    lo.reuse_address = true;
    lo.proxy_protocol = true;

    auto listen_addr = ipv4_addr("127.0.0.1", 12012);
    server_socket ss = seastar::listen(listen_addr, lo);

    bool got_connection = false;

    auto server = seastar::async([&ss, &got_connection] {
        using namespace std::chrono_literals;
        // Set a timer to abort accept after 100ms
        timer<> abort_timer([&ss] {
            ss.abort_accept();
        });
        abort_timer.arm(timer<>::clock::now() + 100ms);

        try {
            auto ar = ss.accept().get();
            got_connection = true;
            ar.connection.shutdown_output();
        } catch (...) {
            // Expected - abort_accept throws
        }
    });

    // IPv4 requires 12 bytes, send header claiming only 8
    auto client = seastar::async([&listen_addr] {
        try {
            auto s = connect(listen_addr).get();
            auto out = s.output();
            unsigned char buf[24];
            const unsigned char sig[12] = {0x0d, 0x0a, 0x0d, 0x0a, 0x00, 0x0d, 0x0a, 0x51, 0x55, 0x49, 0x54, 0x0a};
            std::memcpy(buf, sig, 12);
            buf[12] = 0x21; // v2 PROXY
            buf[13] = 0x11; // IPv4 TCP
            buf[14] = 0x00; // length high byte
            buf[15] = 0x08; // length low byte (8 bytes, too short)
            // Fill in 8 bytes of data
            for (int i = 16; i < 24; ++i) {
                buf[i] = 0;
            }
            out.write(reinterpret_cast<char*>(buf), sizeof(buf)).get();
            out.flush().get();
            out.close().get();
        } catch (...) {
            // ignore
        }
    });

    when_all(std::move(server), std::move(client)).discard_result().get();
    ss.abort_accept();

    BOOST_REQUIRE(!got_connection);
}

SEASTAR_THREAD_TEST_CASE(proxy_protocol_v2_length_too_short_ipv6) {
    listen_options lo;
    lo.reuse_address = true;
    lo.proxy_protocol = true;

    auto listen_addr = ipv6_addr("::1", 12013);
    server_socket ss = seastar::listen(listen_addr, lo);

    bool got_connection = false;

    auto server = seastar::async([&ss, &got_connection] {
        using namespace std::chrono_literals;
        // Set a timer to abort accept after 100ms
        timer<> abort_timer([&ss] {
            ss.abort_accept();
        });
        abort_timer.arm(timer<>::clock::now() + 100ms);

        try {
            auto ar = ss.accept().get();
            got_connection = true;
            ar.connection.shutdown_output();
        } catch (...) {
            // Expected - abort_accept throws
        }
    });

    // IPv6 requires 36 bytes, send header claiming only 20
    auto client = seastar::async([&listen_addr] {
        try {
            auto s = connect(listen_addr).get();
            auto out = s.output();
            std::vector<unsigned char> buf(36);
            const unsigned char sig[12] = {0x0d, 0x0a, 0x0d, 0x0a, 0x00, 0x0d, 0x0a, 0x51, 0x55, 0x49, 0x54, 0x0a};
            std::memcpy(buf.data(), sig, 12);
            buf[12] = 0x21; // v2 PROXY
            buf[13] = 0x21; // IPv6 TCP
            buf[14] = 0x00; // length high byte
            buf[15] = 0x14; // length low byte (20 bytes, too short)
            // Fill in 20 bytes of data
            out.write(reinterpret_cast<char*>(buf.data()), 36).get();
            out.flush().get();
            out.close().get();
        } catch (...) {
            // ignore
        }
    });

    when_all(std::move(server), std::move(client)).discard_result().get();
    ss.abort_accept();

    BOOST_REQUIRE(!got_connection);
}

SEASTAR_THREAD_TEST_CASE(proxy_protocol_v2_with_tlvs) {
    // TLVs should be skipped/ignored by the implementation
    listen_options lo;
    lo.reuse_address = true;
    lo.proxy_protocol = true;

    auto listen_addr = ipv4_addr("127.0.0.1", 12014);
    server_socket ss = seastar::listen(listen_addr, lo);

    auto expected_remote = ipv4_addr("192.168.1.100", 5000);
    auto expected_local = ipv4_addr("10.0.0.1", 8080);

    socket_address actual_remote;
    socket_address actual_local;

    auto server = seastar::async([&ss, &actual_remote, &actual_local] {
        auto ar = ss.accept().get();
        actual_remote = ar.connection.remote_address();
        actual_local = ar.connection.local_address();
        ar.connection.shutdown_output();
    });

    // Add some TLVs (type, length_hi, length_lo, value...)
    std::vector<char> tlvs;
    tlvs.push_back(0x01); // PP2_TYPE_ALPN
    tlvs.push_back(0x00); // length high
    tlvs.push_back(0x08); // length low (8 bytes)
    tlvs.insert(tlvs.end(), 8, 'x'); // dummy data

    auto header = make_proxy_v2_header(true, 0x21, 0x11, expected_remote, expected_local, tlvs);
    auto client = seastar::async([&listen_addr, header = std::move(header)] {
        auto s = connect(listen_addr).get();
        auto out = s.output();
        out.write(header.data(), header.size()).get();
        out.flush().get();
        out.close().get();
    });

    when_all(std::move(server), std::move(client)).discard_result().get();
    ss.abort_accept();

    // Verify addresses still work correctly despite TLVs
    BOOST_REQUIRE_EQUAL(actual_remote, socket_address(expected_remote));
    BOOST_REQUIRE_EQUAL(actual_local, socket_address(expected_local));
}

SEASTAR_THREAD_TEST_CASE(proxy_protocol_v2_extreme_ports) {
    // Test with port 0 and port 65535
    listen_options lo;
    lo.reuse_address = true;
    lo.proxy_protocol = true;

    auto listen_addr = ipv4_addr("127.0.0.1", 12015);
    server_socket ss = seastar::listen(listen_addr, lo);

    auto expected_remote = ipv4_addr("192.168.1.100", 0);      // port 0
    auto expected_local = ipv4_addr("10.0.0.1", 65535);        // port 65535

    socket_address actual_remote;
    socket_address actual_local;

    auto server = seastar::async([&ss, &actual_remote, &actual_local] {
        auto ar = ss.accept().get();
        actual_remote = ar.connection.remote_address();
        actual_local = ar.connection.local_address();
        ar.connection.shutdown_output();
    });

    auto header = make_proxy_v2_header(true, 0x21, 0x11, expected_remote, expected_local);
    auto client = seastar::async([&listen_addr, header = std::move(header)] {
        auto s = connect(listen_addr).get();
        auto out = s.output();
        out.write(header.data(), header.size()).get();
        out.flush().get();
        out.close().get();
    });

    when_all(std::move(server), std::move(client)).discard_result().get();
    ss.abort_accept();

    BOOST_REQUIRE_EQUAL(actual_remote.port(), 0);
    BOOST_REQUIRE_EQUAL(actual_local.port(), 65535);
}

SEASTAR_THREAD_TEST_CASE(proxy_protocol_v2_unspec_family) {
    // UNSPEC family (0x0) should only be valid with LOCAL command
    auto header = make_proxy_v2_header(true, 0x21, 0x00);  // v2 PROXY with UNSPEC (invalid combo)
    test_proxy_header_negative(ipv4_addr("127.0.0.1", 12016), std::move(header));
}
