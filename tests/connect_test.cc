#include "tests/test-utils.hh"

#include "net/ip.hh"

using namespace net;

SEASTAR_TEST_CASE(test_connection_attempt_is_shutdown) {
    ipv4_addr server_addr("172.16.0.1");
    auto unconn = engine().net().socket();
    auto f = unconn
        .connect(make_ipv4_address(server_addr))
        .then_wrapped([] (auto&& f) {
            try {
                f.get();
                BOOST_REQUIRE(false);
            } catch (...) {}
        });
    unconn.shutdown();
    return f;
}

SEASTAR_TEST_CASE(test_unconnected_socket_shutsdown_established_connection) {
    auto sa = make_ipv4_address({"127.0.0.1", 10001});
    return do_with(engine().net().listen(sa, listen_options()), [sa] (auto& listener) {
        listener.accept();
        auto unconn = engine().net().socket();
        auto connf = unconn.connect(sa);
        return connf.then([unconn = std::move(unconn)] (auto&& conn) mutable {
            unconn.shutdown();
            return do_with(std::move(conn), [] (auto& conn) {
                return do_with(conn.output(1), [] (auto& out) {
                    return out.write("ping").then_wrapped([] (auto&& f) {
                        try {
                            f.get();
                            BOOST_REQUIRE(false);
                        } catch (...) {}
                    });
                });
            });
        });
    });
}
