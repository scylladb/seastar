// Seastar's syscall thread: measures how long a file operation that runs
// there takes, and shows that a slow one delays this shard's later file
// operations, but not the shard's other work, or another shard's file
// operations.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/file.hh>
#include <seastar/core/loop.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/smp.hh>
#include <seastar/core/temporary_buffer.hh>
#include <boost/program_options.hpp>
#include <fmt/format.h>
#include <sys/stat.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <random>
#include <ranges>

using namespace seastar;
using namespace std::chrono_literals;
using clk = std::chrono::steady_clock;

double us_since(clk::time_point start) {
    return std::chrono::duration<double, std::micro>(clk::now() - start).count();
}

// snippet: cost
// How long file_size(), which makes the stat() system call in the syscall
// thread, takes, and how long stat() takes when the shard makes it itself.
future<> measure_cost(std::string name, int n) {
    auto start = clk::now();
    for (int i = 0; i < n; ++i) {
        co_await file_size(name);
    }
    double in_thread = us_since(start) / n;
    start = clk::now();
    for (int i = 0; i < n; ++i) {
        struct stat st;
        ::stat(name.c_str(), &st);  // blocks the shard, only briefly here
    }
    double in_shard = us_since(start) / n;
    fmt::print("file_size(): {:.1f} us, stat() in the shard: {:.1f} us\n",
               in_thread, in_shard);
}
// end snippet: cost

// Writes a file of n blocks of 4 KB, with a hole of 4 KB after each, so
// that the file system stores each block as a separate piece of the file.
future<> write_pieces(std::string name, int n) {
    auto f = co_await open_file_dma(name, open_flags::wo |
        open_flags::create | open_flags::truncate);
    constexpr size_t block = 4096;
    auto buf = temporary_buffer<char>::aligned(f.memory_dma_alignment(), block);
    std::mt19937_64 random(0);
    for (size_t i = 0; i < block; i += 8) {
        auto r = random();
        std::memcpy(buf.get_write() + i, &r, 8);
    }
    co_await max_concurrent_for_each(std::views::iota(0, n), 64,
            [&] (int i) -> future<> {
        co_await f.dma_write(uint64_t(i) * 2 * block, buf.get(), block);
    });
    co_await f.flush();
    co_await f.close();
}

// snippet: slow
// Calls file_size() again and again, until another shard sets done, and
// returns the longest that one call took, in microseconds.
future<double> slowest_file_size(std::string name,
                                 const std::atomic<bool>& done) {
    double slowest = 0;
    while (!done) {
        auto start = clk::now();
        co_await file_size(name);
        slowest = std::max(slowest, us_since(start));
    }
    co_return slowest;
}

// Removes a file of many pieces, which takes the file system a while, and
// meanwhile times a file_size() on this shard and file_size() on shard 1,
// and counts the sleeps of 1 ms that this shard finishes.
future<> remove_slowly(std::string pieces, std::string small) {
    auto start = clk::now();
    auto removed = remove_file(pieces).then([start] { return us_since(start); });
    auto here = file_size(small).then([start] (uint64_t) {
        return us_since(start);
    });
    std::atomic<bool> done = false;     // set when the removal ends
    auto there = smp::submit_to(1, [small, &done] {
        return slowest_file_size(small, done);
    });
    int sleeps = 0;
    while (!removed.available()) {
        co_await sleep(1ms);
        ++sleeps;
    }
    done = true;
    fmt::print("remove_file(): {:.0f} ms\n", co_await std::move(removed) / 1000);
    fmt::print("file_size() on this shard, started with it: {:.0f} ms\n",
               co_await std::move(here) / 1000);
    fmt::print("slowest file_size() on shard 1, meanwhile: {:.2f} ms\n",
               co_await std::move(there) / 1000);
    fmt::print("sleeps of 1 ms on this shard, meanwhile: {}\n", sleeps);
}
// end snippet: slow

int main(int argc, char** argv) {
    namespace bpo = boost::program_options;
    app_template app;
    app.add_options()
        ("dir", bpo::value<std::string>()->default_value("."),
         "the directory for the program's two files, which it removes")
        ("pieces", bpo::value<int>()->default_value(200000),
         "how many pieces the file to remove has")
        ("calls", bpo::value<int>()->default_value(100000),
         "how many calls to time");
    return app.run(argc, argv, [&app] -> future<> {
        auto& config = app.configuration();
        auto dir = config["dir"].as<std::string>();
        auto pieces = config["pieces"].as<int>();
        auto n = config["calls"].as<int>();
        if (this_smp_shard_count() < 2) {
            throw std::runtime_error("run with at least two shards (-c2)");
        }
        auto small = dir + "/syscall-thread-small.dat";
        auto in_pieces = dir + "/syscall-thread-pieces.dat";
        co_await write_pieces(small, 1);
        co_await measure_cost(small, n);
        co_await write_pieces(in_pieces, pieces);
        co_await remove_slowly(in_pieces, small);
        co_await remove_file(small);
    });
}
