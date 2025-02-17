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
using namespace std::literals::string_view_literals;

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
        websocket::server_connection conn(dummy, acceptor.get().connection);
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
        SEASTAR_ASSERT(resp);
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
class test_source_impl : public data_source_impl {
    std::vector<temporary_buffer<char>> _bufs{};
    size_t _idx = 0;
public:
    void push_back(std::string s) {
        auto buf = temporary_buffer<char>::copy_of(s);
        _bufs.emplace_back(std::move(buf));
    }
    virtual future<temporary_buffer<char>> get() override {
        if (_idx < _bufs.size()) {
            return make_ready_future<temporary_buffer<char>>(_bufs[_idx++].share());
        }
        return make_ready_future<temporary_buffer<char>>(temporary_buffer<char>{});
    }
};

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

            auto source = std::make_unique<test_source_impl>();

            if (split_i == 0) {
                source->push_back(ws_frames);
            } else {
                source->push_back(ws_frames.substr(0, split_i));
                source->push_back(ws_frames.substr(split_i));
            }

            input_stream<char> in{data_source{std::move(source)}};

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
