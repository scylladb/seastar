// Random reads from a file: writes a file, and then reads blocks of it at
// random offsets, with more and more reads in flight at once, and reports
// how many it did a second, and how long each took.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/file.hh>
#include <seastar/core/loop.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/temporary_buffer.hh>
#include <boost/program_options.hpp>
#include <fmt/format.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <random>
#include <ranges>
#include <vector>

using namespace seastar;
using clk = std::chrono::steady_clock;

// snippet: write
// Fills the file with size bytes, a megabyte at a time, from a buffer
// aligned as the file needs.
future<> fill(file& f, uint64_t size) {
    constexpr size_t chunk = 1 << 20;
    auto buf = temporary_buffer<char>::aligned(f.memory_dma_alignment(), chunk);
    // Random bytes, which a file system that compresses files, as
    // btrfs may, can't make smaller.
    std::mt19937_64 random(0);
    for (size_t i = 0; i < chunk; i += 8) {
        auto r = random();
        std::memcpy(buf.get_write() + i, &r, 8);
    }
    for (uint64_t pos = 0; pos < size; pos += chunk) {
        co_await f.dma_write(pos, buf.get(), chunk);
    }
    co_await f.flush();
}
// end snippet: write

// snippet: read
// Reads n blocks of block_size bytes, at random offsets that are multiples
// of the block's size, with at most in_flight reads at once.
future<> read_randomly(file& f, uint64_t size, size_t block_size, int n,
                       int in_flight) {
    std::mt19937_64 random(in_flight);
    std::uniform_int_distribution<uint64_t> block(0, size / block_size - 1);
    std::vector<double> ms;     // how long each read took
    auto start = clk::now();
    co_await max_concurrent_for_each(std::views::iota(0, n), in_flight,
            [&] (int) -> future<> {
        auto read_start = clk::now();
        auto buf = co_await f.dma_read<char>(block(random) * block_size,
                                             block_size);
        ms.push_back(std::chrono::duration<double, std::milli>(
            clk::now() - read_start).count());
    });
    auto seconds = std::chrono::duration<double>(clk::now() - start).count();
    std::ranges::sort(ms);
    fmt::print("{:3} in flight: {:7.0f} reads a second, each {:.3f} ms on "
               "average, {:.3f} ms for the slowest 1%\n", in_flight,
               n / seconds, std::ranges::fold_left(ms, 0.0, std::plus<>()) / n,
               ms[ms.size() * 99 / 100]);
}
// end snippet: read

int main(int argc, char** argv) {
    namespace bpo = boost::program_options;
    app_template app;
    app.add_options()
        ("file", bpo::value<std::string>()->default_value("random-reads.dat"),
         "the file to write, read, and remove")
        ("size", bpo::value<uint64_t>()->default_value(1024),
         "the file's size, in megabytes")
        ("reads", bpo::value<int>()->default_value(20000),
         "how many reads to do, for each number in flight")
        ("block", bpo::value<size_t>()->default_value(4096),
         "the size of each read, in bytes");
    return app.run(argc, argv, [&app] -> future<> {
        auto& config = app.configuration();
        auto name = config["file"].as<std::string>();
        auto size = config["size"].as<uint64_t>() << 20;
        auto n = config["reads"].as<int>();
        auto block = config["block"].as<size_t>();
        // snippet: open
        auto f = co_await open_file_dma(name, open_flags::rw |
            open_flags::create | open_flags::truncate);
        // end snippet: open
        co_await fill(f, size);
        for (int in_flight : {1, 4, 16, 64}) {
            co_await read_randomly(f, size, block, n, in_flight);
        }
        co_await f.close();
        co_await remove_file(name);
    });
}
