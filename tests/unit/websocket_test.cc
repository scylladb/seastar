/*
 * Copyright 2021 ScyllaDB
 */

#include <chrono>
#include <limits>
#include <utility>
#include <seastar/core/future.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/temporary_buffer.hh>
#include <seastar/core/when_all.hh>
#include <seastar/core/with_timeout.hh>
#include <seastar/net/ip.hh>
#include <seastar/websocket/server.hh>
#include <seastar/websocket/client.hh>
#include <seastar/testing/test_case.hh>
#include <seastar/testing/thread_test_case.hh>
#include <seastar/http/response_parser.hh>
#include <seastar/core/byteorder.hh>
#include <seastar/util/defer.hh>
#include <seastar/util/memory-data-source.hh>
#include <system_error>
#include <stdexcept>
#include "loopback_socket.hh"

using namespace seastar;
using namespace seastar::experimental;
using namespace std::chrono_literals;
using namespace std::literals::string_view_literals;

namespace seastar::experimental::websocket {

template <bool is_client, bool text_frame>
struct basic_connection_test_accessor {
    using connection_type = basic_connection<is_client, text_frame>;

    static future<> send_data(connection_type& conn, opcodes opcode, temporary_buffer<char> buff) {
        return conn.send_data(opcode, std::move(buff));
    }

    static void poison_send_chain(connection_type& conn, std::exception_ptr ex) {
        conn._send_chain = make_exception_future<>(ex);
    }

    static future<> drain_send_chain_error(connection_type& conn) {
        return std::exchange(conn._send_chain, make_ready_future<>()).handle_exception([] (std::exception_ptr) {});
    }

    static void set_close_timeout(connection_type& conn, lowres_clock::duration timeout) {
        conn._close_timeout = timeout;
    }

    static input_stream<char>& input(connection_type& conn) {
        return conn._input;
    }

    static output_stream<char>& output(connection_type& conn) {
        return conn._output;
    }
};

}


class eof_data_source_impl : public data_source_impl {
public:
    future<temporary_buffer<char>> get() override {
        return make_ready_future<temporary_buffer<char>>(temporary_buffer<char>());
    }
};

class counting_data_sink_impl : public data_sink_impl {
    unsigned& _put_attempts;

public:
    explicit counting_data_sink_impl(unsigned& put_attempts)
        : _put_attempts(put_attempts) {
    }

#if SEASTAR_API_LEVEL >= 9
    future<> put(std::span<temporary_buffer<char>>) override {
#else
    future<> put(net::packet) override {
#endif
        ++_put_attempts;
        return make_ready_future<>();
    }

    future<> close() override {
        return make_ready_future<>();
    }
};

class close_failing_data_sink_impl : public data_sink_impl {
public:
#if SEASTAR_API_LEVEL >= 9
    future<> put(std::span<temporary_buffer<char>>) override {
#else
    future<> put(net::packet) override {
#endif
        return make_ready_future<>();
    }

    future<> close() override {
        return make_exception_future<>(std::runtime_error("output close failed"));
    }
};

class write_failing_data_sink_impl : public data_sink_impl {
public:
#if SEASTAR_API_LEVEL >= 9
    future<> put(std::span<temporary_buffer<char>>) override {
#else
    future<> put(net::packet) override {
#endif
        return make_exception_future<>(std::runtime_error("write failed"));
    }

    future<> close() override {
        return make_ready_future<>();
    }
};

// A data source that yields one frame then EOF. Used by Fix K test.
class close_then_eof_source_impl : public data_source_impl {
    std::string _frame;
    bool _sent = false;
public:
    explicit close_then_eof_source_impl(std::string frame) : _frame(std::move(frame)) {}
    future<temporary_buffer<char>> get() override {
        if (!_sent) {
            _sent = true;
            return make_ready_future<temporary_buffer<char>>(temporary_buffer<char>::copy_of(_frame));
        }
        return make_ready_future<temporary_buffer<char>>(temporary_buffer<char>());
    }
};

// A connected_socket_impl whose source yields a single frame then EOF
// and whose sink always fails writes (to exercise Fix B).
class close_source_write_failing_socket_impl : public net::connected_socket_impl {
    std::string _frame;
public:
    explicit close_source_write_failing_socket_impl(std::string frame) : _frame(std::move(frame)) {}
    data_source source() override {
        return data_source(std::make_unique<close_then_eof_source_impl>(_frame));
    }
    data_sink sink() override {
        return data_sink(std::make_unique<write_failing_data_sink_impl>());
    }
    void shutdown_input() override {}
    void shutdown_output() override {}
    void set_nodelay(bool) override {}
    bool get_nodelay() const override { return true; }
    void set_keepalive(bool) override {}
    bool get_keepalive() const override { return false; }
    void set_keepalive_parameters(const net::keepalive_params&) override {}
    net::keepalive_params get_keepalive_parameters() const override {
        return net::tcp_keepalive_params{std::chrono::seconds(0), std::chrono::seconds(0), 0};
    }
    void set_sockopt(int, int, const void*, size_t) override {
        throw std::runtime_error("not supported");
    }
    int get_sockopt(int, int, void*, size_t) const override {
        throw std::runtime_error("not supported");
    }
    socket_address local_address() const noexcept override { return {}; }
    socket_address remote_address() const noexcept override { return {}; }
    future<> wait_input_shutdown() override { return make_ready_future<>(); }
};

class counting_connected_socket_impl : public net::connected_socket_impl {
    unsigned& _put_attempts;

public:
    explicit counting_connected_socket_impl(unsigned& put_attempts)
        : _put_attempts(put_attempts) {
    }

    data_source source() override {
        return data_source(std::make_unique<eof_data_source_impl>());
    }

    data_sink sink() override {
        return data_sink(std::make_unique<counting_data_sink_impl>(_put_attempts));
    }

    void shutdown_input() override {
    }

    void shutdown_output() override {
    }

    void set_nodelay(bool) override {
    }

    bool get_nodelay() const override {
        return true;
    }

    void set_keepalive(bool) override {
    }

    bool get_keepalive() const override {
        return false;
    }

    void set_keepalive_parameters(const net::keepalive_params&) override {
    }

    net::keepalive_params get_keepalive_parameters() const override {
        return net::tcp_keepalive_params {std::chrono::seconds(0), std::chrono::seconds(0), 0};
    }

    void set_sockopt(int, int, const void*, size_t) override {
        throw std::runtime_error("Setting custom socket options is not supported");
    }

    int get_sockopt(int, int, void*, size_t) const override {
        throw std::runtime_error("Getting custom socket options is not supported");
    }

    socket_address local_address() const noexcept override {
        return {};
    }

    socket_address remote_address() const noexcept override {
        return {};
    }

    future<> wait_input_shutdown() override {
        return make_ready_future<>();
    }
};

template <typename Source, typename Sink>
class test_connected_socket_impl : public net::connected_socket_impl {
public:
    data_source source() override {
        return data_source(std::make_unique<Source>());
    }

    data_sink sink() override {
        return data_sink(std::make_unique<Sink>());
    }

    void shutdown_input() override {
    }

    void shutdown_output() override {
    }

    void set_nodelay(bool) override {
    }

    bool get_nodelay() const override {
        return true;
    }

    void set_keepalive(bool) override {
    }

    bool get_keepalive() const override {
        return false;
    }

    void set_keepalive_parameters(const net::keepalive_params&) override {
    }

    net::keepalive_params get_keepalive_parameters() const override {
        return net::tcp_keepalive_params {std::chrono::seconds(0), std::chrono::seconds(0), 0};
    }

    void set_sockopt(int, int, const void*, size_t) override {
        throw std::runtime_error("Setting custom socket options is not supported");
    }

    int get_sockopt(int, int, void*, size_t) const override {
        throw std::runtime_error("Getting custom socket options is not supported");
    }

    socket_address local_address() const noexcept override {
        return {};
    }

    socket_address remote_address() const noexcept override {
        return {};
    }

    future<> wait_input_shutdown() override {
        return make_ready_future<>();
    }
};

std::string buffer_to_string(const temporary_buffer<char>& buf) {
    return std::string(buf.begin(), buf.end());
}

std::string make_websocket_frame(uint8_t opcode, std::string_view payload = {}, bool masked = false, bool fin = true,
        uint32_t masking_key = 0) {
    std::string frame;
    frame.push_back(static_cast<char>((fin ? 0x80 : 0) | opcode));

    uint8_t mask_bit = masked ? 0x80 : 0;
    if (payload.size() < 126) {
        frame.push_back(static_cast<char>(mask_bit | payload.size()));
    } else if (payload.size() <= std::numeric_limits<uint16_t>::max()) {
        frame.push_back(static_cast<char>(mask_bit | 126));
        char len[sizeof(uint16_t)];
        write_be<uint16_t>(len, payload.size());
        frame.append(len, sizeof(len));
    } else {
        frame.push_back(static_cast<char>(mask_bit | 127));
        char len[sizeof(uint64_t)];
        write_be<uint64_t>(len, payload.size());
        frame.append(len, sizeof(len));
    }

    if (masked) {
        char mask[sizeof(uint32_t)];
        write_be<uint32_t>(mask, masking_key);
        frame.append(mask, sizeof(mask));
        auto masked_payload = std::string(payload);
        for (size_t i = 0; i < masked_payload.size(); ++i) {
            masked_payload[i] ^= mask[i % sizeof(mask)];
        }
        frame.append(masked_payload);
    } else {
        frame.append(payload);
    }
    return frame;
}

std::string make_websocket_frame_header(uint8_t opcode, uint64_t payload_size, bool masked = false, bool fin = true) {
    std::string frame;
    frame.push_back(static_cast<char>((fin ? 0x80 : 0) | opcode));

    uint8_t mask_bit = masked ? 0x80 : 0;
    if (payload_size < 126) {
        frame.push_back(static_cast<char>(mask_bit | payload_size));
    } else if (payload_size <= std::numeric_limits<uint16_t>::max()) {
        frame.push_back(static_cast<char>(mask_bit | 126));
        char len[sizeof(uint16_t)];
        write_be<uint16_t>(len, payload_size);
        frame.append(len, sizeof(len));
    } else {
        frame.push_back(static_cast<char>(mask_bit | 127));
        char len[sizeof(uint64_t)];
        write_be<uint64_t>(len, payload_size);
        frame.append(len, sizeof(len));
    }

    if (masked) {
        frame.append(4, '\0');
    }
    return frame;
}

std::string build_request(std::string_view key_base64, std::string_view subprotocol) {
    std::string subprotocol_line;
    if (!subprotocol.empty()) {
        subprotocol_line = fmt::format("Sec-WebSocket-Protocol: {}\r\n", subprotocol);
    }

    return fmt::format(
        "GET / HTTP/1.1\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: {}\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "{}"
        "\r\n",
        key_base64,
        subprotocol_line);
}

future<> test_websocket_handshake_common(std::string subprotocol) {
    return seastar::async([=] {
        const std::string request = build_request("dGhlIHNhbXBsZSBub25jZQ==", subprotocol);

        loopback_connection_factory factory;
        loopback_socket_impl lsi(factory);

        auto acceptor = factory.get_server_socket().accept();
        auto connector = lsi.connect(socket_address(), socket_address());
        connected_socket sock = connector.get();
        auto input = sock.input();
        auto output = sock.output();

        websocket::server dummy;
        dummy.register_handler(subprotocol, [] (input_stream<char>& in,
                        output_stream<char>& out) {
                return repeat([&in, &out]() {
                    return in.read().handle_exception_type([] (std::system_error& e) {
                            BOOST_REQUIRE_EQUAL(e.code().value(), static_cast<int>(std::errc::broken_pipe));
                            return make_ready_future<temporary_buffer<char>>(temporary_buffer<char>());
                        }).then([&out](temporary_buffer<char> f) {
                        if (f.empty()) {
                            return make_ready_future<stop_iteration>(stop_iteration::yes);
                        } else {
                            return out.write(std::move(f)).then([&out]() {
                                return out.flush().then([] {
                                    return make_ready_future<stop_iteration>(stop_iteration::no);
                                });
                            });
                        }
                    });
                });
            });
        websocket::server_connection conn(dummy, acceptor.get().connection);
        future<> serve = conn.process();
        auto close = defer([&conn, &input, &output, &serve] () noexcept {
            conn.close(false).get();
            input.close().get();
            output.close().get();
            serve.get();
         });

        // Send the handshake
        output.write(request).get();
        output.flush().get();
        // Check that the server correctly computed the response
        // according to WebSocket handshake specification
        http_response_parser parser;
        parser.init();
        input.consume(parser).get();
        std::unique_ptr<http::reply> resp = parser.get_parsed_response();
        SEASTAR_ASSERT(resp);
        sstring websocket_accept = resp->_headers["Sec-WebSocket-Accept"];
        // Trim possible whitespace prefix
        auto it = std::find_if(websocket_accept.begin(), websocket_accept.end(), ::isalnum);
        if (it != websocket_accept.end()) {
            websocket_accept.erase(websocket_accept.begin(), it);
        }
        BOOST_REQUIRE_EQUAL(websocket_accept, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
    });
}

SEASTAR_TEST_CASE(test_websocket_handshake) {
    return test_websocket_handshake_common("echo");
}

SEASTAR_TEST_CASE(test_websocket_handshake_no_subprotocol) {
    return test_websocket_handshake_common("");
}

future<> test_websocket_handler_registration_common(std::string subprotocol) {
    return seastar::async([=] {
        loopback_connection_factory factory;
        loopback_socket_impl lsi(factory);

        auto acceptor = factory.get_server_socket().accept();
        auto connector = lsi.connect(socket_address(), socket_address());
        connected_socket sock = connector.get();
        auto input = sock.input();
        auto output = sock.output();

        // Setup server
        websocket::server ws;
        ws.register_handler(subprotocol, [] (input_stream<char>& in,
                        output_stream<char>& out) {
            return repeat([&in, &out]() {
                return in.read().handle_exception_type([] (std::system_error& e) {
                            BOOST_REQUIRE_EQUAL(e.code().value(), static_cast<int>(std::errc::broken_pipe));
                            return make_ready_future<temporary_buffer<char>>(temporary_buffer<char>());
                        }).then([&out](temporary_buffer<char> f) {
                            if (f.empty()) {
                                return make_ready_future<stop_iteration>(stop_iteration::yes);
                            } else {
                                return out.write(std::move(f)).then([&out]() {
                                    return out.flush().then([] {
                                        return make_ready_future<stop_iteration>(stop_iteration::no);
                                    });
                                });
                            }
                });
            });
        });
        websocket::server_connection conn(ws, acceptor.get().connection);
        future<> serve = conn.process();

        auto close = defer([&conn, &input, &output, &serve] () noexcept {
            conn.close().get();
            input.close().get();
            output.close().get();
            serve.get();
         });

        // handshake
        const std::string request = build_request("dGhlIHNhbXBsZSBub25jZQ==", subprotocol);
        output.write(request).get();
        output.flush().get();

        unsigned reply_size = 156;
        if (!subprotocol.empty()) {
            reply_size += ("\r\nSec-WebSocket-Protocol: "sv).size() + subprotocol.size();
        }
        input.read_exactly(reply_size).get();

        unsigned ws_frame_len = 10;

        // Sending and receiving a websocket frame
        const std::string ws_frame = std::string(
            "\202\204"  // 1000 0002 1000 0100
            "TEST"      // Masking Key
            "\0\0\0\0", ws_frame_len); // Masked Message - TEST
        const auto rs_frame = std::string(
            "\202\004" // 1000 0002 0000 0100
            "TEST", 6);    // Message - TEST

        output.write(ws_frame).get();
        output.flush().get();

        auto response = input.read_exactly(6).get();
        auto response_str = std::string(response.begin(), response.end());
        BOOST_REQUIRE_EQUAL(rs_frame, response_str);
    });
}

SEASTAR_TEST_CASE(test_websocket_handler_registration) {
    return test_websocket_handler_registration_common("echo");
}

SEASTAR_TEST_CASE(test_websocket_handler_registration_no_subprotocol) {
    return test_websocket_handler_registration_common("");
}

// Simple wrapper to help create a testable input_stream.

SEASTAR_TEST_CASE(test_websocket_parser_split) {
    return seastar::async([] {
        // Two websocket frames
        std::string ws_frames = std::string(
            "\x82\x85"    // FIN, opcode, mask, payload len (5)
            "\0\0\0\0"    // Masking Key
            "TEST1"       // payload
            "\x82\x85"    // FIN, opcode, mask, payload len (5)
            "\0\0\0\0"    // Masking Key
            "TEST2"
            "\x82\x85"    // FIN, opcode, mask, payload len (5)
            "\0\0\0\0"    // Masking Key
            "TEST3", 33); // payload

        for (unsigned split_i = 0; split_i < ws_frames.size() - 1; ++split_i) {
            websocket::websocket_parser parser;

            auto bufs = std::vector<temporary_buffer<char>>{};

            if (split_i == 0) {
                bufs.push_back(temporary_buffer<char>::copy_of(ws_frames));
            } else {
                bufs.push_back(temporary_buffer<char>::copy_of(ws_frames.substr(0, split_i)));
                bufs.push_back(temporary_buffer<char>::copy_of(ws_frames.substr(split_i)));
            }

            input_stream<char> in = util::as_input_stream(std::move(bufs));

            std::vector<sstring> results;

            while (true) {
                in.consume(parser).get();
                if (parser.eof()) {
                    break;
                }

                SEASTAR_ASSERT(parser.is_valid());
                results.push_back(seastar::to_sstring(parser.result()));
            }

            SEASTAR_ASSERT(!parser.is_valid());
            BOOST_REQUIRE_EQUAL(0, parser.result().size());

            std::vector<sstring> expected = {
                "TEST1",
                "TEST2",
                "TEST3",
            };

            BOOST_REQUIRE_EQUAL(results, expected);
        }
    });
}

SEASTAR_TEST_CASE(test_websocket_parser_rejects_unknown_opcodes) {
    return seastar::async([] {
        const std::vector<uint8_t> unknown_opcodes = {
            0x3, 0x4, 0x5, 0x6, 0x7,
            0xB, 0xC, 0xD, 0xE, 0xF,
        };

        for (auto opcode : unknown_opcodes) {
            websocket::websocket_parser parser;
            auto bufs = std::vector<temporary_buffer<char>>{};
            bufs.push_back(temporary_buffer<char>::copy_of(make_websocket_frame(opcode, {}, true)));
            input_stream<char> in = util::as_input_stream(std::move(bufs));

            in.consume(parser).get();

            BOOST_REQUIRE_MESSAGE(!parser.is_valid(), fmt::format("opcode 0x{:x} was accepted", opcode));
            BOOST_REQUIRE(!parser.eof());
            BOOST_REQUIRE_EQUAL(parser.opcode(), static_cast<websocket::opcodes>(opcode));
        }
    });
}

SEASTAR_TEST_CASE(test_websocket_parser_rejects_oversized_control_frames) {
    return seastar::async([] {
        websocket::websocket_parser parser;
        auto bufs = std::vector<temporary_buffer<char>>{};
        bufs.push_back(temporary_buffer<char>::copy_of(make_websocket_frame(websocket::opcodes::PING, std::string(126, 'x'), true)));
        input_stream<char> in = util::as_input_stream(std::move(bufs));

        in.consume(parser).get();

        BOOST_REQUIRE(!parser.is_valid());
        BOOST_REQUIRE(!parser.eof());
        BOOST_REQUIRE_EQUAL(parser.opcode(), websocket::opcodes::PING);
    });
}

SEASTAR_TEST_CASE(test_websocket_parser_rejects_fragmented_control_frames) {
    return seastar::async([] {
        websocket::websocket_parser parser;
        auto bufs = std::vector<temporary_buffer<char>>{};
        bufs.push_back(temporary_buffer<char>::copy_of(make_websocket_frame(websocket::opcodes::CLOSE, {}, true, false)));
        input_stream<char> in = util::as_input_stream(std::move(bufs));

        in.consume(parser).get();

        BOOST_REQUIRE(!parser.is_valid());
        BOOST_REQUIRE(!parser.eof());
        BOOST_REQUIRE_EQUAL(parser.opcode(), websocket::opcodes::CLOSE);
    });
}

SEASTAR_TEST_CASE(test_websocket_parser_rejects_huge_declared_data_frame_before_allocating) {
    return seastar::async([] {
        websocket::websocket_parser parser;
        auto bufs = std::vector<temporary_buffer<char>>{};
        bufs.push_back(temporary_buffer<char>::copy_of(make_websocket_frame_header(websocket::opcodes::BINARY, uint64_t(1) << 60, true)));
        input_stream<char> in = util::as_input_stream(std::move(bufs));

        in.consume(parser).get();

        BOOST_REQUIRE(!parser.is_valid());
        BOOST_REQUIRE(!parser.eof());
        BOOST_REQUIRE_EQUAL(parser.opcode(), websocket::opcodes::BINARY);
    });
}

SEASTAR_TEST_CASE(test_websocket_parser_respects_configured_data_frame_limit) {
    return seastar::async([] {
        {
            websocket::websocket_parser parser(true, 5);
            auto bufs = std::vector<temporary_buffer<char>>{};
            bufs.push_back(temporary_buffer<char>::copy_of(make_websocket_frame(websocket::opcodes::BINARY, "12345", true)));
            input_stream<char> in = util::as_input_stream(std::move(bufs));

            in.consume(parser).get();

            BOOST_REQUIRE(parser.is_valid());
            BOOST_REQUIRE_EQUAL(buffer_to_string(parser.result()), "12345");
        }
        {
            websocket::websocket_parser parser(true, 5);
            auto bufs = std::vector<temporary_buffer<char>>{};
            bufs.push_back(temporary_buffer<char>::copy_of(make_websocket_frame_header(websocket::opcodes::BINARY, 6, true)));
            input_stream<char> in = util::as_input_stream(std::move(bufs));

            in.consume(parser).get();

            BOOST_REQUIRE(!parser.is_valid());
            BOOST_REQUIRE(!parser.eof());
            BOOST_REQUIRE_EQUAL(parser.opcode(), websocket::opcodes::BINARY);
        }
    });
}

SEASTAR_TEST_CASE(test_websocket_close_timer_fires_when_peer_never_echoes_close) {
    loopback_connection_factory factory;
    loopback_socket_impl lsi(factory);

    auto acceptor = factory.get_server_socket().accept();
    auto connector = lsi.connect(socket_address(), socket_address());

    auto server_sock = (co_await std::move(acceptor)).connection;
    auto client_sock = co_await std::move(connector);
    auto client_input = client_sock.input();
    auto client_output = client_sock.output();

    using Conn = websocket::basic_connection<false, false>;
    using Acc = websocket::basic_connection_test_accessor<false, false>;
    Conn conn(std::move(server_sock));
    Acc::set_close_timeout(conn, 50ms);
    auto start = lowres_clock::now();
    auto close_f = conn.close(true);

    auto close_frame = co_await client_input.read_exactly(2);
    BOOST_REQUIRE_EQUAL(buffer_to_string(close_frame), make_websocket_frame(websocket::opcodes::CLOSE));

    co_await with_timeout(lowres_clock::now() + 2s, std::move(close_f));
    auto elapsed = lowres_clock::now() - start;
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    BOOST_REQUIRE_GE(elapsed_ms, 50);
    BOOST_REQUIRE_LT(elapsed_ms, 500);

    co_await client_input.close();
    co_await client_output.close();
}

SEASTAR_TEST_CASE(test_websocket_close_handshake_send_failure_tears_down) {
    using socket_impl = test_connected_socket_impl<eof_data_source_impl, write_failing_data_sink_impl>;
    using Conn = websocket::basic_connection<false, false>;
    using Acc = websocket::basic_connection_test_accessor<false, false>;
    Conn conn(connected_socket(std::make_unique<socket_impl>()));

    co_await conn.close(true).then_wrapped([] (future<> f) {
        BOOST_REQUIRE(f.failed());
        try {
            f.get();
        } catch (const std::runtime_error& e) {
            BOOST_REQUIRE_EQUAL(std::string(e.what()), "write failed");
        }
    });

    auto buf = co_await Acc::input(conn).read();
    BOOST_REQUIRE(buf.empty());
}

SEASTAR_TEST_CASE(test_websocket_send_chain_failure_is_terminal_until_drained) {
    using Conn = websocket::basic_connection<false, false>;
    using Acc = websocket::basic_connection_test_accessor<false, false>;
    unsigned put_attempts = 0;
    Conn conn(connected_socket(std::make_unique<counting_connected_socket_impl>(put_attempts)));
    Acc::poison_send_chain(conn, std::make_exception_ptr(std::runtime_error("test poisoned send chain")));

    co_await Acc::send_data(conn, websocket::opcodes::BINARY, temporary_buffer<char>::copy_of("after poison")).then_wrapped([] (future<> f) {
        BOOST_REQUIRE(f.failed());
        try {
            f.get();
        } catch (const std::runtime_error& e) {
            BOOST_REQUIRE_EQUAL(std::string(e.what()), "test poisoned send chain");
        }
    });

    BOOST_REQUIRE_EQUAL(put_attempts, 0);

    co_await Acc::drain_send_chain_error(conn);
    co_await Acc::send_data(conn, websocket::opcodes::BINARY, temporary_buffer<char>::copy_of("after drain"));
    BOOST_REQUIRE_EQUAL(put_attempts, 1);

    co_await conn.close(false).handle_exception([] (std::exception_ptr) {});
}

SEASTAR_TEST_CASE(test_websocket_concurrent_send_data_frames_are_not_interleaved) {
    loopback_connection_factory factory;
    loopback_socket_impl lsi(factory);

    auto acceptor = factory.get_server_socket().accept();
    auto connector = lsi.connect(socket_address(), socket_address());

    auto server_sock = (co_await std::move(acceptor)).connection;
    auto client_sock = co_await std::move(connector);
    auto client_input = client_sock.input();
    auto client_output = client_sock.output();

    using Conn = websocket::basic_connection<false, false>;
    using Acc = websocket::basic_connection_test_accessor<false, false>;
    Conn conn(std::move(server_sock));
    const std::string first_payload(9000, 'a');
    const std::string second_payload(9000, 'b');

    auto first_send = Acc::send_data(conn, websocket::opcodes::BINARY, temporary_buffer<char>::copy_of(first_payload));
    auto second_send = Acc::send_data(conn, websocket::opcodes::BINARY, temporary_buffer<char>::copy_of(second_payload));

    const auto expected = make_websocket_frame(websocket::opcodes::BINARY, first_payload)
        + make_websocket_frame(websocket::opcodes::BINARY, second_payload);
    auto read_f = client_input.read_exactly(expected.size());

    co_await when_all_succeed(std::move(first_send), std::move(second_send));
    auto raw_frames = co_await std::move(read_f);
    BOOST_REQUIRE_EQUAL(buffer_to_string(raw_frames), expected);

    co_await conn.close(false);
    co_await client_input.close();
    co_await client_output.close();
}

SEASTAR_TEST_CASE(test_websocket_input_unmasks_non_zero_masked_frame) {
    loopback_connection_factory factory;
    loopback_socket_impl lsi(factory);

    auto acceptor = factory.get_server_socket().accept();
    auto connector = lsi.connect(socket_address(), socket_address());

    auto server_sock = (co_await std::move(acceptor)).connection;
    auto client_sock = co_await std::move(connector);
    auto client_input = client_sock.input();
    auto client_output = client_sock.output();

    using Conn = websocket::basic_connection<false, false>;
    using Acc = websocket::basic_connection_test_accessor<false, false>;
    Conn conn(std::move(server_sock));
    auto read_f = Acc::input(conn).read();
    BOOST_REQUIRE(!read_f.available());

    co_await client_output.write(make_websocket_frame(websocket::opcodes::BINARY,
            "masked payload", true, true, 0x12345678));
    co_await client_output.flush();

    auto buf = co_await std::move(read_f);
    BOOST_REQUIRE_EQUAL(buffer_to_string(buf), "masked payload");

    co_await conn.close(false);
    co_await client_input.close();
    co_await client_output.close();
}

SEASTAR_TEST_CASE(test_websocket_peer_close_while_handler_is_reading) {
    loopback_connection_factory factory;
    loopback_socket_impl lsi(factory);

    auto acceptor = factory.get_server_socket().accept();
    auto connector = lsi.connect(socket_address(), socket_address());

    auto server_sock = (co_await std::move(acceptor)).connection;
    auto client_sock = co_await std::move(connector);
    auto client_input = client_sock.input();
    auto client_output = client_sock.output();

    using Conn = websocket::basic_connection<false, false>;
    using Acc = websocket::basic_connection_test_accessor<false, false>;
    Conn conn(std::move(server_sock));
    auto read_f = Acc::input(conn).read();
    BOOST_REQUIRE(!read_f.available());

    co_await client_output.write(make_websocket_frame(websocket::opcodes::CLOSE, {}, true));
    co_await client_output.flush();

    auto close_frame = co_await client_input.read_exactly(2);
    BOOST_REQUIRE_EQUAL(buffer_to_string(close_frame), make_websocket_frame(websocket::opcodes::CLOSE));

    auto buf = co_await std::move(read_f);
    BOOST_REQUIRE(buf.empty());

    co_await Acc::output(conn).close();

    co_await client_input.close();
    co_await client_output.close();
}

SEASTAR_TEST_CASE(test_websocket_invalid_frame_while_handler_is_reading_reports_error_once) {
    loopback_connection_factory factory;
    loopback_socket_impl lsi(factory);

    auto acceptor = factory.get_server_socket().accept();
    auto connector = lsi.connect(socket_address(), socket_address());

    auto server_sock = (co_await std::move(acceptor)).connection;
    auto client_sock = co_await std::move(connector);
    auto client_input = client_sock.input();
    auto client_output = client_sock.output();

    using Conn = websocket::basic_connection<false, false>;
    using Acc = websocket::basic_connection_test_accessor<false, false>;
    Conn conn(std::move(server_sock));
    auto read_f = Acc::input(conn).read();
    BOOST_REQUIRE(!read_f.available());

    co_await client_output.write(make_websocket_frame(0x3, {}, true));
    co_await client_output.flush();

    co_await std::move(read_f).then_wrapped([] (future<temporary_buffer<char>> f) {
        BOOST_REQUIRE(f.failed());
        try {
            f.get();
        } catch (const websocket::exception& e) {
            BOOST_REQUIRE_EQUAL(std::string(e.what()), "Reading from socket has failed.");
        }
    });

    auto buf = co_await Acc::input(conn).read();
    BOOST_REQUIRE(buf.empty());

    auto eof = co_await client_input.read();
    BOOST_REQUIRE(eof.empty());

    co_await client_input.close();
    co_await client_output.close();
}

SEASTAR_TEST_CASE(test_websocket_wait_close_stops_after_invalid_frame) {
    loopback_connection_factory factory;
    loopback_socket_impl lsi(factory);

    auto acceptor = factory.get_server_socket().accept();
    auto connector = lsi.connect(socket_address(), socket_address());

    auto server_sock = (co_await std::move(acceptor)).connection;
    auto client_sock = co_await std::move(connector);
    auto client_input = client_sock.input();
    auto client_output = client_sock.output();

    using Conn = websocket::basic_connection<false, false>;
    Conn conn(std::move(server_sock));
    auto close_f = conn.close(true);

    auto close_frame = co_await client_input.read_exactly(2);
    BOOST_REQUIRE_EQUAL(buffer_to_string(close_frame), make_websocket_frame(websocket::opcodes::CLOSE));

    co_await client_output.write(make_websocket_frame(0x3, {}, true));
    co_await client_output.close();

    co_await with_timeout(lowres_clock::now() + 2s, std::move(close_f));
    co_await client_input.close();
}

SEASTAR_TEST_CASE(test_websocket_close2_double_close) {
    loopback_connection_factory factory;
    loopback_socket_impl lsi(factory);

    auto acceptor = factory.get_server_socket().accept();
    auto connector = lsi.connect(socket_address(), socket_address());

    auto server_sock = (co_await std::move(acceptor)).connection;
    auto client_sock = co_await std::move(connector);
    auto client_input = client_sock.input();
    auto client_output = client_sock.output();

    using Conn = websocket::basic_connection<false, false>;
    Conn conn(std::move(server_sock));
    auto first_close = conn.close(false);
    auto second_close = conn.close(false);

    co_await when_all_succeed(std::move(first_close), std::move(second_close));

    co_await client_input.close();
    co_await client_output.close();
}

SEASTAR_TEST_CASE(test_websocket_close2_double_close_handshake_sends_one_close) {
    loopback_connection_factory factory;
    loopback_socket_impl lsi(factory);

    auto acceptor = factory.get_server_socket().accept();
    auto connector = lsi.connect(socket_address(), socket_address());

    auto server_sock = (co_await std::move(acceptor)).connection;
    auto client_sock = co_await std::move(connector);
    auto client_input = client_sock.input();
    auto client_output = client_sock.output();

    using Conn = websocket::basic_connection<false, false>;
    Conn conn(std::move(server_sock));
    auto first_close = conn.close(true);
    auto second_close = conn.close(true);

    auto close_frame = co_await client_input.read_exactly(2);
    BOOST_REQUIRE_EQUAL(buffer_to_string(close_frame), make_websocket_frame(websocket::opcodes::CLOSE));

    co_await client_output.write(make_websocket_frame(websocket::opcodes::CLOSE, {}, true));
    co_await client_output.flush();

    co_await when_all_succeed(std::move(first_close), std::move(second_close));

    auto extra = co_await client_input.read();
    BOOST_REQUIRE(extra.empty());

    co_await client_input.close();
    co_await client_output.close();
}

SEASTAR_TEST_CASE(test_websocket_close_while_consuming_sends_close_frame_and_tears_down) {
    loopback_connection_factory factory;
    loopback_socket_impl lsi(factory);

    auto acceptor = factory.get_server_socket().accept();
    auto connector = lsi.connect(socket_address(), socket_address());

    auto server_sock = (co_await std::move(acceptor)).connection;
    auto client_sock = co_await std::move(connector);
    auto client_input = client_sock.input();
    auto client_output = client_sock.output();

    using Conn = websocket::basic_connection<false, false>;
    using Acc = websocket::basic_connection_test_accessor<false, false>;
    Conn conn(std::move(server_sock));
    auto read_f = Acc::input(conn).read();
    BOOST_REQUIRE(!read_f.available());

    auto close_f = conn.close(true);

    auto close_frame = co_await client_input.read_exactly(2);
    BOOST_REQUIRE_EQUAL(buffer_to_string(close_frame), make_websocket_frame(websocket::opcodes::CLOSE));

    co_await with_timeout(lowres_clock::now() + 2s, std::move(close_f));

    co_await std::move(read_f).then_wrapped([] (future<temporary_buffer<char>> f) {
        BOOST_REQUIRE(f.failed());
        try {
            f.get();
            BOOST_FAIL("read should fail after direct teardown");
        } catch (const std::system_error& e) {
            BOOST_REQUIRE_EQUAL(e.code().value(), static_cast<int>(std::errc::broken_pipe));
        }
    });

    co_await client_input.close();
    co_await client_output.close();
}

SEASTAR_TEST_CASE(test_websocket_client_shutdown_before_connect) {
    websocket::client<> ws_client;
    ws_client.shutdown();
    co_await ws_client.close();
}

SEASTAR_TEST_CASE(test_websocket_client_shutdown_during_connect_handshake) {
    auto listener = seastar::listen(make_ipv4_address({"127.0.0.1", 0}), listen_options());
    auto accept_f = listener.accept();

    websocket::client<> ws_client;
    auto connect_f = ws_client.connect(listener.local_address(), "/", "localhost", "",
            [] (input_stream<char>&, output_stream<char>&) {
        return make_ready_future<>();
    });

    auto server_sock = (co_await std::move(accept_f)).connection;
    auto server_input = server_sock.input();
    auto server_output = server_sock.output();

    auto request = co_await server_input.read();
    BOOST_REQUIRE(!request.empty());

    ws_client.shutdown();
    co_await with_timeout(lowres_clock::now() + 2s, std::move(connect_f).then_wrapped([] (future<> f) {
        BOOST_REQUIRE(f.failed());
        f.ignore_ready_future();
    }));
    co_await ws_client.close();

    co_await server_input.close();
    co_await server_output.close();
    listener.abort_accept();
}

SEASTAR_TEST_CASE(test_websocket_process_propagates_handler_exception) {
    loopback_connection_factory factory;
    loopback_socket_impl lsi(factory);

    auto acceptor = factory.get_server_socket().accept();
    auto connector = lsi.connect(socket_address(), socket_address());

    auto server_sock = (co_await std::move(acceptor)).connection;
    auto client_sock = co_await std::move(connector);
    auto client_input = client_sock.input();
    auto client_output = client_sock.output();

    websocket::server ws;
    ws.register_handler("", [] (input_stream<char>&, output_stream<char>&) {
        return make_exception_future<>(std::runtime_error("handler failed"));
    });

    websocket::server_connection conn(ws, std::move(server_sock));
    auto serve = conn.process();

    co_await client_output.write(build_request("dGhlIHNhbXBsZSBub25jZQ==", ""));
    co_await client_output.flush();
    co_await client_input.read_exactly(156);

    co_await std::move(serve).then_wrapped([] (future<> f) {
        BOOST_REQUIRE(f.failed());
        try {
            f.get();
        } catch (const std::runtime_error& e) {
            BOOST_REQUIRE_EQUAL(std::string(e.what()), "handler failed");
        }
    });

    co_await conn.close(false);
    co_await client_input.close();
    co_await client_output.close();
}

SEASTAR_TEST_CASE(test_websocket_client_process_preserves_handler_exception_when_output_close_fails) {
    using socket_impl = test_connected_socket_impl<eof_data_source_impl, close_failing_data_sink_impl>;
    websocket::client_connection conn(connected_socket(std::make_unique<socket_impl>()),
            "/", "localhost", "",
            [] (input_stream<char>&, output_stream<char>&) {
        return make_exception_future<>(std::runtime_error("handler failed"));
    });

    co_await conn.process().then_wrapped([] (future<> f) {
        BOOST_REQUIRE(f.failed());
        try {
            f.get();
        } catch (const std::runtime_error& e) {
            BOOST_REQUIRE_EQUAL(std::string(e.what()), "handler failed");
        }
    });

    co_await conn.close(false).handle_exception([] (std::exception_ptr) {});
}

SEASTAR_TEST_CASE(test_websocket_client_server) {
    loopback_connection_factory factory;
    loopback_socket_impl lsi(factory);

    // Setup server side
    websocket::server ws;
    ws.register_handler("echo", [] (input_stream<char>& in,
                    output_stream<char>& out) -> future<> {
        while (true) {
            auto f = co_await in.read();
            if (f.empty()) {
                break;
            }

            co_await out.write(std::move(f));
            co_await out.flush();
        }
    });

    auto acceptor = factory.get_server_socket().accept();
    auto connector = lsi.connect(socket_address(), socket_address());

    // Server side
    connected_socket server_sock = (co_await std::move(acceptor)).connection;
    websocket::server_connection server_conn(ws, std::move(server_sock));
    auto serve = server_conn.process();

    // Client side
    connected_socket client_sock = co_await std::move(connector);

    sstring received_data;
    promise<> client_done;

    websocket::client_connection client_conn(std::move(client_sock), "/", "localhost",
        "echo",
        [&received_data, &client_done] (input_stream<char>& in, output_stream<char>& out) -> future<> {
            co_await out.write("hello");
            co_await out.flush();

            auto buf = co_await in.read();
            received_data = sstring(buf.get(), buf.size());
            co_await out.close();
            client_done.set_value();
        });

    co_await client_conn.handshake();
    auto client_process = client_conn.process().handle_exception(
        [] (std::exception_ptr) {});

    co_await client_done.get_future();
    BOOST_REQUIRE_EQUAL(received_data, "hello");

    co_await std::move(serve);
    co_await std::move(client_process);
    co_await client_conn.close(false);
    co_await server_conn.close(false);
}

SEASTAR_TEST_CASE(test_websocket_server_stop_shuts_down_active_connections) {
    loopback_connection_factory factory;
    loopback_socket_impl lsi(factory);

    websocket::server ws;
    promise<> handler_blocked;
    promise<> handler_done;
    ws.register_handler("", [&handler_blocked, &handler_done] (input_stream<char>& in, output_stream<char>&) -> future<> {
        handler_blocked.set_value();
        try {
            co_await in.read();
        } catch (...) {
            // expected: broken pipe or other error when server stops
        }
        handler_done.set_value();
    });

    ws.listen(factory.get_server_socket());

    // Connect a raw client and complete the WebSocket handshake
    auto connector = lsi.connect(socket_address(), socket_address());
    connected_socket sock = co_await std::move(connector);
    auto client_input = sock.input();
    auto client_output = sock.output();

    co_await client_output.write(build_request("dGhlIHNhbXBsZSBub25jZQ==", ""));
    co_await client_output.flush();

    // Consume the 101 response
    http_response_parser parser;
    parser.init();
    co_await client_input.consume(parser);
    BOOST_REQUIRE(parser.get_parsed_response());

    // Wait until the handler is blocked inside in.read()
    co_await handler_blocked.get_future();

    // Stop the server — should shut down active connections
    co_await with_timeout(lowres_clock::now() + 2s, ws.stop());

    // Handler should have observed EOF/error and returned
    co_await with_timeout(lowres_clock::now() + 2s, handler_done.get_future());

    co_await client_input.close();
    co_await client_output.close();
}

SEASTAR_TEST_CASE(test_websocket_respond_to_peer_close_write_failure_still_tears_down) {
    // Build a CLOSE frame that the server-side parser (expecting masked frames) will accept.
    std::string close_frame = make_websocket_frame(websocket::opcodes::CLOSE, {}, /*masked=*/true);

    using Conn = websocket::basic_connection<false, false>;
    using Acc  = websocket::basic_connection_test_accessor<false, false>;
    Conn conn2(connected_socket(std::make_unique<close_source_write_failing_socket_impl>(close_frame)));

    // read() triggers consume() → CLOSE frame → respond_to_peer_close()
    // respond_to_peer_close() tries to send CLOSE → write_failing_data_sink_impl → handle_exception swallows
    // → tear_down_streams() runs → _pending_inbound is set to empty → read() returns empty.
    // With fix B applied, the exception is swallowed and teardown completes.
    auto buf = co_await with_timeout(lowres_clock::now() + 2s, Acc::input(conn2).read());
    BOOST_REQUIRE(buf.empty());

    // A second read must also return empty (not hang), confirming teardown completed.
    auto buf2 = co_await with_timeout(lowres_clock::now() + 2s, Acc::input(conn2).read());
    BOOST_REQUIRE(buf2.empty());
}

SEASTAR_TEST_CASE(test_websocket_close_from_within_handler_does_not_deadlock) {
    loopback_connection_factory factory2;
    loopback_socket_impl lsi2(factory2);

    auto acceptor2 = factory2.get_server_socket().accept();
    auto connector2 = lsi2.connect(socket_address(), socket_address());
    auto server_sock2 = (co_await std::move(acceptor2)).connection;
    auto client_sock2 = co_await std::move(connector2);
    auto client_input2 = client_sock2.input();
    auto client_output2 = client_sock2.output();

    using Conn = websocket::basic_connection<false, false>;
    using Acc  = websocket::basic_connection_test_accessor<false, false>;
    Conn conn2(std::move(server_sock2));
    Acc::set_close_timeout(conn2, 200ms);

    // Start a handler that reads one message then closes output.
    // Closing the output triggers close(true), which exercises the _in_consume path.
    auto handler_f = Acc::input(conn2).read().then([&conn2] (temporary_buffer<char>) {
        return Acc::output(conn2).close();
    });

    // Send a data frame from the client side
    co_await client_output2.write(make_websocket_frame(websocket::opcodes::BINARY, "hi", /*masked=*/true));
    co_await client_output2.flush();

    // handler_f should complete (close drives the CLOSE handshake via _in_consume path)
    // with_timeout ensures we don't hang if the _in_consume guard is broken
    co_await with_timeout(lowres_clock::now() + 2s, std::move(handler_f));

    co_await conn2.close(false).handle_exception([] (std::exception_ptr) {});
    co_await client_input2.close();
    co_await client_output2.close();
}

SEASTAR_TEST_CASE(test_websocket_client_shutdown_unblocks_blocked_handler) {
    auto listener = seastar::listen(make_ipv4_address({"127.0.0.1", 0}), listen_options());

    websocket::client<> ws_client;
    promise<> handler_reading;
    auto connect_f = ws_client.connect(listener.local_address(), "/", "localhost", "",
            [&handler_reading] (input_stream<char>& in, output_stream<char>&) -> future<> {
        handler_reading.set_value();
        co_await in.read(); // blocks until shutdown
    });

    // Accept the TCP connection and send a valid 101 upgrade response manually
    auto ar = co_await listener.accept();
    auto server_input  = ar.connection.input();
    auto server_output = ar.connection.output();

    // Read and discard the upgrade request
    auto req_buf = co_await server_input.read();
    BOOST_REQUIRE(!req_buf.empty());

    // Parse the Sec-WebSocket-Key header from req_buf.
    sstring req_str(req_buf.get(), req_buf.size());
    auto key_pos = req_str.find("Sec-WebSocket-Key: ");
    BOOST_REQUIRE(key_pos != sstring::npos);
    auto key_start = key_pos + 19;
    auto key_end = req_str.find("\r\n", key_start);
    BOOST_REQUIRE(key_end != sstring::npos);
    sstring ws_key = req_str.substr(key_start, key_end - key_start);

    auto accept_val = websocket::sha1_base64(fmt::format("{}{}", ws_key, websocket::websocket_magic_guid));
    auto response_101 = fmt::format(
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: {}\r\n"
        "\r\n",
        accept_val);

    co_await server_output.write(response_101);
    co_await server_output.flush();

    // Wait until the handler is blocked on in.read()
    co_await with_timeout(lowres_clock::now() + 2s, handler_reading.get_future());

    // shutdown() + close() should both resolve
    ws_client.shutdown();
    co_await with_timeout(lowres_clock::now() + 2s, ws_client.close());

    co_await server_input.close();
    co_await server_output.close();
    listener.abort_accept();
}
