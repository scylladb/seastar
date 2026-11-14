// Shoal's micro-benchmarks (version 16), with Seastar's perf_tests: each
// PERF_TEST is run again and again, and its time per iteration measured.
// Run them with "build/shoal_perf_tests -c1 -m1G --blocked-reactor-notify-ms
// 2000": a test whose future is ready at once runs, again and again, for a
// whole run, a second, without yielding, which the stall detector would
// report.
#include "commands.hh"
#include "resp.hh"
#include "table.hh"
#include <seastar/core/coroutine.hh>
#include <seastar/testing/perf_tests.hh>
#include <seastar/util/memory-data-source.hh>
#include <fmt/format.h>
#include <limits>
#include <string>
#include <vector>

logger shoal_log("shoal");

// snippet: perf-parse
// A pipeline of 100 GET requests, in RESP.
static const std::string pipeline = [] {
    std::string s;
    for (int i = 0; i < 100; i++) {
        auto key = fmt::format("key:{:06}", i);
        s += fmt::format("*2\r\n$3\r\nGET\r\n${}\r\n{}\r\n", key.size(), key);
    }
    return s;
}();

// Parses the pipeline, from one buffer, as a connection's reader would,
// and returns the number of requests.
future<size_t> parse_pipeline() {
    std::vector<temporary_buffer<char>> buffers;
    buffers.emplace_back(pipeline.data(), pipeline.size());
    auto in = util::as_input_stream(std::move(buffers));
    semaphore memory(std::numeric_limits<int32_t>::max());
    request_reader reader(in, memory, memory.available_units());
    size_t requests = 0;
    while (auto req = co_await reader.read()) {
        perf_tests::do_not_optimize(*req);
        requests++;
    }
    co_return requests;
}

// A test whose function returns the number of things it did (here, in a
// future) has the time of each reported, rather than of the whole. The
// test's own function can't be a coroutine, since its return type is
// deduced: it calls one.
PERF_TEST(resp, parse_pipeline) {
    return parse_pipeline();
}
// end snippet: perf-parse

// snippet: perf-table
// A table of a million keys, which a fixture, a class whose constructor
// runs once before the test's iterations, fills.
struct table_fixture {
    key_table<int> table;
    std::vector<std::string> keys;
    size_t next = 0;    // where the next iteration's lookups start
    table_fixture() {
        for (int i = 0; i < 1'000'000; i++) {
            keys.push_back(fmt::format("key:{:07}", i));
            table.try_emplace(keys.back());
        }
    }
};

// Looks up 1,000 keys, scattered over the table; each iteration goes on
// from where the last stopped, so that the keys aren't in the CPU's cache.
PERF_TEST_F(table_fixture, find) {
    for (size_t i = 0; i < 1000; i++) {
        next = (next + 7919) % keys.size();
        perf_tests::do_not_optimize(table.find(keys[next]));
    }
    return 1000;
}
// end snippet: perf-table
