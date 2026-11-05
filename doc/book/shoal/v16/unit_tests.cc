// Shoal's unit tests (version 16): tests of its parts, each in a Seastar
// program of their own, without a server or a client. Run them with
// "build/shoal_unit_tests -- -c2 -m1G"; Boost.Test's options come before
// the "--", and Seastar's after it.
#define SEASTAR_TESTING_MAIN
#include "commands.hh"
#include "feed.hh"
#include "latency.hh"
#include "resp.hh"
#include <seastar/core/coroutine.hh>
#include <seastar/core/sharded.hh>
#include <seastar/testing/test_case.hh>
#include <seastar/testing/test_fixture.hh>
#include <seastar/testing/thread_test_case.hh>
#include <seastar/util/memory-data-source.hh>
#include <boost/test/unit_test.hpp>
#include <chrono>
#include <limits>
#include <string_view>
#include <vector>

using namespace std::chrono_literals;

// Shoal's logger, which main.cc defines in the server.
logger shoal_log("shoal");

// snippet: boost-test
// A test that needs no reactor: a plain Boost.Test test case, which the
// same program runs.
BOOST_AUTO_TEST_CASE(test_latency_buckets) {
    latency_histogram h;
    h.add(1us);
    h.add(3us);
    h.add(1s);
    auto m = h.to_metrics();
    BOOST_REQUIRE_EQUAL(m.sample_count, 3);
    BOOST_REQUIRE_EQUAL(m.buckets[0].count, 1);   // up to 1 µs
    BOOST_REQUIRE_EQUAL(m.buckets[2].count, 2);   // up to 4 µs
    BOOST_REQUIRE_EQUAL(m.buckets.back().count, 3);
}
// end snippet: boost-test

// snippet: reader-test
// Parses the input, given in these pieces, and returns its requests.
future<std::vector<request>> parse(std::vector<std::string_view> pieces) {
    std::vector<temporary_buffer<char>> buffers;
    for (auto piece : pieces) {
        // An empty buffer would be the end of the stream.
        if (!piece.empty()) {
            buffers.emplace_back(piece.data(), piece.size());
        }
    }
    auto in = util::as_input_stream(std::move(buffers));
    semaphore memory(std::numeric_limits<int32_t>::max());
    request_reader reader(in, memory, memory.available_units());
    std::vector<request> requests;
    while (auto req = co_await reader.read()) {
        requests.push_back(std::move(*req));
    }
    co_return requests;
}

// A test that runs in the reactor: its body is a coroutine. The parser
// must find the same requests wherever the input is cut in two.
SEASTAR_TEST_CASE(test_reader_cut_anywhere) {
    std::string_view input = "*2\r\n$3\r\nGET\r\n$3\r\nkey\r\n*1\r\n$4\r\nPING\r\n";
    std::vector<request> expected{{"GET", "key"}, {"PING"}};
    for (size_t cut = 0; cut <= input.size(); cut++) {
        auto requests = co_await parse({input.substr(0, cut),
                                        input.substr(cut)});
        BOOST_REQUIRE(requests == expected);
    }
}
// end snippet: reader-test

SEASTAR_TEST_CASE(test_reader_protocol_error) {
    BOOST_REQUIRE_EXCEPTION(co_await parse({"*x\r\n"}), protocol_error,
        testing::exception_predicate::message_contains("multibulk"));
}

// snippet: thread-test
// A test in a Seastar thread, which may wait with get(): a fetch from a
// feed that has nothing new waits until its deadline, and a feed that
// dropped the change at an offset says so.
SEASTAR_THREAD_TEST_CASE(test_feed_fetch) {
    replication_feed feed(64 * 1024);
    auto start = lowres_clock::now();
    BOOST_REQUIRE(feed.fetch(0, 1000, start + 50ms).get().empty());
    BOOST_REQUIRE(lowres_clock::now() - start >= 40ms);
    feed.append(std::initializer_list<std::string_view>{"SET", "k", "v"});
    BOOST_REQUIRE_EQUAL(feed.fetch(0, 1000, start).get(),
                        "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n");
    // Ten times the feed's limit pushes the first change out.
    for (int i = 0; i < 10000; i++) {
        feed.append(std::initializer_list<std::string_view>{
            "SET", "key", std::string(50, 'x')});
    }
    BOOST_REQUIRE_EXCEPTION(feed.fetch(0, 1000, start).get(),
        std::runtime_error,
        testing::exception_predicate::message_contains("no longer has"));
}
// end snippet: thread-test

// snippet: fixture
// The data of all the shards, which a test that uses this fixture finds
// started before it runs, and empty, and which is stopped after it, even
// if it fails.
struct shoal_fixture {
    // A test is a const member function of a class derived from the
    // fixture, so what it changes must be mutable.
    mutable sharded<store> db;
    mutable abort_source never;

    future<> setup() {
        // Real scheduling groups aren't needed in tests.
        co_await db.start(scheduling_groups{
            .keys = default_scheduling_group(),
            .rewrite = default_scheduling_group()});
    }
    future<> teardown() {
        co_await db.stop();
    }
    // Executes a request, made of these words, and returns its reply.
    future<sstring> run(request req) const {
        auto r = co_await execute(db, std::move(req), never);
        co_return std::get<sstring>(*r);
    }
};

SEASTAR_FIXTURE_TEST_CASE(test_keys_on_every_shard, shoal_fixture) {
    request mset{"MSET"};
    for (int i = 0; i < 100; i++) {
        mset.push_back(fmt::format("key{}", i));
        mset.push_back(fmt::format("value{}", i));
    }
    BOOST_REQUIRE_EQUAL(co_await run(std::move(mset)), "+OK\r\n");
    BOOST_REQUIRE_EQUAL(co_await run({"DBSIZE"}), ":100\r\n");
    BOOST_REQUIRE_EQUAL(co_await run({"GET", "key42"}),
                        "$7\r\nvalue42\r\n");
    // Each shard has its part of the keys, all of them.
    auto keys = co_await db.map_reduce0([] (store& s) {
        return s.data.size();
    }, size_t(0), std::plus<size_t>());
    BOOST_REQUIRE_EQUAL(keys, 100);
}
// end snippet: fixture
