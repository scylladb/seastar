// Reads and writes: how long a random read of 4 KB takes, alone, and
// while another fiber writes, as fast as it can, in large blocks. The
// reads and the writes are in scheduling groups of their own, whose
// shares the options set, and which the I/O scheduler serves in
// proportion to them.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/file.hh>
#include <seastar/core/loop.hh>
#include <seastar/core/scheduling.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/temporary_buffer.hh>
#include <seastar/core/with_scheduling_group.hh>
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

// Fills the file with size bytes, a megabyte at a time.
future<> fill(file& f, uint64_t size) {
    constexpr size_t chunk = 1 << 20;
    auto buf = temporary_buffer<char>::aligned(f.memory_dma_alignment(),
                                               chunk);
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

// snippet: reader
// Reads 4 KB at random places, with in_flight reads at once, until the
// time is up, and prints how many it did, and how long they took.
future<> read_randomly(file& f, uint64_t size, int in_flight,
                       clk::duration duration) {
    std::mt19937_64 random(1);
    std::uniform_int_distribution<uint64_t> block(0, size / 4096 - 1);
    std::vector<double> ms;
    auto start = clk::now();
    co_await parallel_for_each(std::views::iota(0, in_flight),
                               [&] (int) -> future<> {
        while (clk::now() < start + duration) {
            auto read_start = clk::now();
            co_await f.dma_read<char>(block(random) * 4096, 4096);
            ms.push_back(std::chrono::duration<double, std::milli>(
                clk::now() - read_start).count());
        }
    });
    auto seconds = std::chrono::duration<double>(clk::now() - start).count();
    std::ranges::sort(ms);
    fmt::print("  reads: {:.0f} a second, {:.3f} ms on average, {:.3f} ms "
               "for the slowest 1%\n", ms.size() / seconds,
               std::ranges::fold_left(ms, 0.0, std::plus<>()) / ms.size(),
               ms[ms.size() * 99 / 100]);
}
// end snippet: reader

// snippet: writer
// Writes blocks over the file, from start to end and again, with
// in_flight writes at once, until stop is set; returns the bytes written.
future<uint64_t> write_constantly(file& f, uint64_t size, size_t block,
                                  int in_flight, const bool& stop) {
    auto buf = temporary_buffer<char>::aligned(f.memory_dma_alignment(),
                                               block);
    std::mt19937_64 random(1);
    for (size_t i = 0; i < block; i += 8) {
        auto r = random();
        std::memcpy(buf.get_write() + i, &r, 8);
    }
    uint64_t next = 0, written = 0;
    co_await parallel_for_each(std::views::iota(0, in_flight),
                               [&] (int) -> future<> {
        while (!stop) {
            auto pos = next;
            next = (next + block) % size;
            written += co_await f.dma_write(pos, buf.get(), block);
        }
    });
    co_return written;
}
// end snippet: writer

int main(int argc, char** argv) {
    namespace bpo = boost::program_options;
    app_template app;
    app.add_options()
        ("dir", bpo::value<std::string>()->default_value("."),
         "where to make the two files, which are removed at the end")
        ("size", bpo::value<uint64_t>()->default_value(1024),
         "each file's size, in megabytes")
        ("seconds", bpo::value<int>()->default_value(10),
         "how long to measure, alone and with the writes")
        ("reads-in-flight", bpo::value<int>()->default_value(1),
         "how many reads to keep in flight")
        ("read-shares", bpo::value<float>()->default_value(1000),
         "the shares of the reads' scheduling group")
        ("write-shares", bpo::value<float>()->default_value(1000),
         "the shares of the writes' scheduling group")
        ("write-size", bpo::value<size_t>()->default_value(128),
         "the size of each write, in kilobytes")
        ("writes-in-flight", bpo::value<int>()->default_value(16),
         "how many writes to keep in flight");
    return app.run(argc, argv, [&app] -> future<> {
        auto& config = app.configuration();
        auto dir = config["dir"].as<std::string>();
        auto size = config["size"].as<uint64_t>() << 20;
        auto duration = std::chrono::seconds(config["seconds"].as<int>());
        auto reads_in_flight = config["reads-in-flight"].as<int>();
        auto flags = open_flags::rw | open_flags::create |
                     open_flags::truncate;
        auto reads = co_await open_file_dma(dir + "/reads.dat", flags);
        auto writes = co_await open_file_dma(dir + "/writes.dat", flags);
        co_await fill(reads, size);
        co_await fill(writes, size);
        // snippet: groups
        auto read_group = co_await create_scheduling_group(
            "reads", config["read-shares"].as<float>());
        auto write_group = co_await create_scheduling_group(
            "writes", config["write-shares"].as<float>());
        // end snippet: groups
        fmt::print("alone:\n");
        co_await with_scheduling_group(read_group, [&] {
            return read_randomly(reads, size, reads_in_flight, duration);
        });
        // snippet: together
        fmt::print("with the writes:\n");
        bool stop = false;
        auto start = clk::now();
        auto written = with_scheduling_group(write_group, [&] {
            return write_constantly(writes, size,
                config["write-size"].as<size_t>() << 10,
                config["writes-in-flight"].as<int>(), stop);
        });
        co_await with_scheduling_group(read_group, [&] {
            return read_randomly(reads, size, reads_in_flight, duration);
        });
        stop = true;
        auto mb = co_await std::move(written) / 1e6;
        fmt::print("  writes: {:.0f} MB a second\n", mb /
                   std::chrono::duration<double>(clk::now() - start).count());
        // end snippet: together
        co_await reads.close();
        co_await writes.close();
        co_await remove_file(dir + "/reads.dat");
        co_await remove_file(dir + "/writes.dat");
    });
}
