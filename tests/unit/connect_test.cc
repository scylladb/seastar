#include <seastar/core/reactor.hh>
#include <seastar/testing/random.hh>
#include <seastar/testing/test_case.hh>
#include <seastar/testing/test_runner.hh>
#include <seastar/net/ip.hh>

using namespace seastar;
using namespace net;

SEASTAR_TEST_CASE(test_connection_attempt_is_shutdown) {
    ipv4_addr server_addr("127.0.0.1");
    auto unconn = make_socket();
    auto f = unconn
        .connect(make_ipv4_address(server_addr))
        .then_wrapped([] (auto&& f) {
            try {
                f.get();
                BOOST_REQUIRE(false);
            } catch (...) {}
        });
    unconn.shutdown();
    return f.finally([unconn = std::move(unconn)] {});
}

SEASTAR_TEST_CASE(test_unconnected_socket_shutsdown_established_connection) {
    // Use a random port to reduce chance of conflict.
    // TODO: retry a few times on failure.
    std::default_random_engine& rnd = testing::local_random_engine;
    auto distr = std::uniform_int_distribution<uint16_t>(12000, 65000);
    auto sa = make_ipv4_address({"127.0.0.1", distr(rnd)});
    return do_with(engine().net().listen(sa, listen_options()), [sa] (auto& listener) {
        auto f = listener.accept();
        auto unconn = make_socket();
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
        }).finally([f = std::move(f)] () mutable {
            return std::move(f);
        });
    });
}

SEASTAR_TEST_CASE(test_accept_after_abort) {
    std::default_random_engine& rnd = testing::local_random_engine;
    auto distr = std::uniform_int_distribution<uint16_t>(12000, 65000);
    auto sa = make_ipv4_address({"127.0.0.1", distr(rnd)});
    return do_with(seastar::server_socket(engine().net().listen(sa, listen_options())), [] (auto& listener) {
        using ftype = future<accept_result>;
        promise<ftype> p;
        future<ftype> done = p.get_future();
        auto f = listener.accept().then_wrapped([&listener, p = std::move(p)] (auto f) mutable {
            f.ignore_ready_future();
            p.set_value(listener.accept());
        });
        listener.abort_accept();
        return done.then([] (ftype f) {
          return f.then_wrapped([] (ftype f) {
            BOOST_REQUIRE(f.failed());
            if (f.available()) {
                f.ignore_ready_future();
            }
          });
        });
    });
}
