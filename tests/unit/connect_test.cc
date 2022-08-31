#include <seastar/core/reactor.hh>
#include <seastar/testing/test_case.hh>
#include <seastar/testing/thread_test_case.hh>
#include <seastar/testing/test_runner.hh>
#include <seastar/net/ip.hh>

using namespace seastar;
using namespace net;

SEASTAR_THREAD_TEST_CASE(test_connection_attempt_is_shutdown) {
    ipv4_addr server_addr("172.16.0.1");
    auto unconn = make_socket();
    auto f = unconn.connect(make_ipv4_address(server_addr));
    unconn.shutdown();
    BOOST_REQUIRE_THROW(f.get(), std::exception);
}

SEASTAR_THREAD_TEST_CASE(test_unconnected_socket_shutsdown_established_connection) {
    // Use a random port to reduce chance of conflict.
    // TODO: retry a few times on failure.
    std::default_random_engine& rnd = testing::local_random_engine;
    auto distr = std::uniform_int_distribution<uint16_t>(12000, 65000);
    auto sa = make_ipv4_address({"127.0.0.1", distr(rnd)});
    auto listener = engine().net().listen(sa, listen_options());
    auto f = listener.accept();
    auto unconn = make_socket();
    auto conn = unconn.connect(sa).get();
    unconn.shutdown();
    auto out = conn.output(1);
    BOOST_REQUIRE_THROW(out.write("ping").get(), std::exception);
    f.get();
}

SEASTAR_THREAD_TEST_CASE(test_accept_after_abort) {
    std::default_random_engine& rnd = testing::local_random_engine;
    auto distr = std::uniform_int_distribution<uint16_t>(12000, 65000);
    auto sa = make_ipv4_address({"127.0.0.1", distr(rnd)});
    auto listener = engine().net().listen(sa, listen_options());
    using ftype = future<accept_result>;
    promise<ftype> p;
    future<ftype> done = p.get_future();
    auto f = listener.accept().then_wrapped([&listener, p = std::move(p)] (auto f) mutable {
        f.ignore_ready_future();
        p.set_value(listener.accept());
    });
    listener.abort_accept();
    ftype done_fut = done.get();
    BOOST_REQUIRE_THROW(done_fut.get(), std::exception);
    f.get();
}
