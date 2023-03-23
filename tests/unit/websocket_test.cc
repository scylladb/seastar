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
        std::unique_ptr<http_response> resp = parser.get_parsed_response();
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
