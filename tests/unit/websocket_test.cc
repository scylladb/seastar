/*
 * Copyright 2021 ScyllaDB
 */

#include <seastar/websocket/server.hh>
#include <seastar/testing/test_case.hh>
#include <seastar/testing/thread_test_case.hh>
#include <seastar/http/response_parser.hh>
#include <seastar/util/defer.hh>
#include "loopback_socket.hh"

using namespace seastar;
using namespace seastar::experimental;

SEASTAR_TEST_CASE(test_websocket_handshake) {
    return seastar::async([] {
        const std::string request =
                "GET / HTTP/1.1\r\n"
                "Upgrade: websocket\r\n"
                "Connection: Upgrade\r\n"
                "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                "Sec-WebSocket-Version: 13\r\n"
                "Sec-WebSocket-Protocol: echo\r\n"
                "\r\n";
        loopback_connection_factory factory;
        loopback_socket_impl lsi(factory);

        auto acceptor = factory.get_server_socket().accept();
        auto connector = lsi.connect(socket_address(), socket_address());
        connected_socket sock = connector.get0();
        auto input = sock.input();
        auto output = sock.output();

        websocket::server dummy;
        dummy.register_handler("echo", [] (input_stream<char>& in,
                        output_stream<char>& out) {
                return repeat([&in, &out]() {
                    return in.read().then([&out](temporary_buffer<char> f) {
                        std::cerr << "f.size(): " << f.size() << "\n";
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
        websocket::connection conn(dummy, acceptor.get0().connection);
        future<> serve = conn.process();
        auto close = defer([&conn, &input, &output, &serve] () noexcept {
            conn.close().get();
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
        BOOST_ASSERT(resp);
        sstring websocket_accept = resp->_headers["Sec-WebSocket-Accept"];
        // Trim possible whitespace prefix
        auto it = std::find_if(websocket_accept.begin(), websocket_accept.end(), ::isalnum);
        if (it != websocket_accept.end()) {
            websocket_accept.erase(websocket_accept.begin(), it);
        }
        BOOST_REQUIRE_EQUAL(websocket_accept, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
        for (auto& header : resp->_headers) {
            std::cout << header.first << ':' << header.second << std::endl;
        }
    });
}



SEASTAR_TEST_CASE(test_websocket_handler_registration) {
    return seastar::async([] {
        loopback_connection_factory factory;
        loopback_socket_impl lsi(factory);

        auto acceptor = factory.get_server_socket().accept();
        auto connector = lsi.connect(socket_address(), socket_address());
        connected_socket sock = connector.get0();
        auto input = sock.input();
        auto output = sock.output();

        // Setup server
        websocket::server ws;
        ws.register_handler("echo", [] (input_stream<char>& in,
                        output_stream<char>& out) {
            return repeat([&in, &out]() {
                return in.read().then([&out](temporary_buffer<char> f) {
                    std::cerr << "f.size(): " << f.size() << "\n";
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
        websocket::connection conn(ws, acceptor.get0().connection);
        future<> serve = conn.process();

        auto close = defer([&conn, &input, &output, &serve] () noexcept {
            conn.close().get();
            input.close().get();
            output.close().get();
            serve.get();
         });

        // handshake
        const std::string request =
                "GET / HTTP/1.1\r\n"
                "Upgrade: websocket\r\n"
                "Connection: Upgrade\r\n"
                "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                "Sec-WebSocket-Version: 13\r\n"
                "Sec-WebSocket-Protocol: echo\r\n"
                "\r\n";
        output.write(request).get();
        output.flush().get();
        input.read_exactly(186).get();

        // Sending and receiving a websocket frame
        const auto ws_frame = std::string(
            "\202\204"  // 1000 0002 1000 0100
            "TEST"      // Masking Key
            "\0\0\0\0", 10); // Masked Message - TEST
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


/*
 * Simple wrapper to help create a testable input_stream.
 */
class test_source_impl : public data_source_impl {
    std::vector<temporary_buffer<char>> _bufs{};
    size_t idx = 0;
    public:
    void push_back(std::string s) {
        auto buf = temporary_buffer<char>::copy_of(s);
        _bufs.emplace_back(std::move(buf));
    }
    virtual future<temporary_buffer<char>> get() override {
        if (idx < _bufs.size()) {
            return make_ready_future<temporary_buffer<char>>(_bufs[idx++].share());
        }
        return make_ready_future<temporary_buffer<char>>(temporary_buffer<char>{});
    }
};

SEASTAR_TEST_CASE(test_websocket_parser) {
    return seastar::async([] {
        websocket::websocket_parser parser{};

        // A complete WebSocket frame with payload.
        std::string ws_frame = std::string(
                "\202\204"   // FIN, opcode, mask, payload len (4)
                "\0\0\0\0"   // Masking Key
                "TEST", 10); // payload
        auto source = std::make_unique<test_source_impl>();
        source->push_back(ws_frame);
        input_stream<char> in{data_source{std::move(source)}};

        // Consume the Frame
        in.consume(parser).get();
        BOOST_ASSERT(parser.is_valid());
        auto result = parser.result();
        const std::string expected = "TEST";
        BOOST_REQUIRE_EQUAL(expected.size(), result.size());
        BOOST_REQUIRE_EQUAL(seastar::to_sstring(expected),
                seastar::to_sstring(std::move(result)));

        // EOF
        in.consume(parser).get();
        BOOST_ASSERT(!parser.is_valid());
        BOOST_REQUIRE_EQUAL(0, parser.result().size());
    });
}

SEASTAR_TEST_CASE(test_websocket_parser_multiple_frames) {
    return seastar::async([] {
        websocket::websocket_parser parser{};

        // Two complete WebSocket frames in a single buffer.
        std::string ws_frames = std::string(
                "\202\205"    // FIN, opcode, mask, payload len (5)
                "\0\0\0\0"    // Masking Key
                "TEST1"       // payload
                "\202\205"    // FIN, opcode, mask, payload len (5)
                "\0\0\0\0"    // Masking Key
                "TEST2", 22); // payload

        auto source = std::make_unique<test_source_impl>();
        source->push_back(ws_frames);
        input_stream<char> in{data_source{std::move(source)}};

        // Frame 1
        in.consume(parser).get();
        BOOST_ASSERT(parser.is_valid());
        auto result1 = parser.result();
        const std::string expected1 = "TEST1";
        BOOST_REQUIRE_EQUAL(expected1.size(), result1.size());
        BOOST_REQUIRE_EQUAL(seastar::to_sstring(expected1),
                seastar::to_sstring(std::move(result1)));

        // Frame 2
        in.consume(parser).get();
        BOOST_ASSERT(parser.is_valid());
        auto result2 = parser.result();
        const std::string expected2 = "TEST2";
        BOOST_REQUIRE_EQUAL(expected2.size(), result2.size());
        BOOST_REQUIRE_EQUAL(seastar::to_sstring(expected2),
                seastar::to_sstring(std::move(result2)));

        // EOF
        in.consume(parser).get();
        BOOST_ASSERT(!parser.is_valid());
        BOOST_REQUIRE_EQUAL(0, parser.result().size());
    });
}

SEASTAR_TEST_CASE(test_websocket_parser_with_split_payload) {
    return seastar::async([] {
        websocket::websocket_parser parser{};

        // A complete WebSocket frame with payload, but with part of a
        // subsequent frame at the end.
        std::string ws_frame1 = std::string(
                "\202\205" // FIN, opcode, mask, payload len (5)
                "\0\0\0\0" // Masking key (no-op)
                "TE", 8);  // Payload part 1

        std::string ws_frame2 = "ST2"; // Payload part 2

        auto source = std::make_unique<test_source_impl>();
        source->push_back(ws_frame1);
        source->push_back(ws_frame2);
        input_stream<char> in{data_source{std::move(source)}};

        // Incomplete Frame
        in.consume(parser).get();
        BOOST_ASSERT(parser.is_valid());
        BOOST_REQUIRE_EQUAL(0, parser.result().size());

        // End of completed Frame
        in.consume(parser).get();
        BOOST_ASSERT(parser.is_valid());
        auto result = parser.result();
        const std::string expected = "TEST2";
        BOOST_REQUIRE_EQUAL(expected.size(), result.size());
        BOOST_REQUIRE_EQUAL(seastar::to_sstring(expected),
                seastar::to_sstring(std::move(result)));

        // EOF
        in.consume(parser).get();
        BOOST_ASSERT(!parser.is_valid());
        BOOST_REQUIRE_EQUAL(0, parser.result().size());
    });
}
