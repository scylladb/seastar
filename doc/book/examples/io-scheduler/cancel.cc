// Cancelling I/O: starts 100 random reads of a megabyte at once, all
// with one io_intent, cancels them a millisecond later, and counts how
// many were done and how many were cancelled.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/file.hh>
#include <seastar/core/io_intent.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/temporary_buffer.hh>
#include <fmt/format.h>
#include <chrono>
#include <cstring>
#include <random>
#include <vector>

using namespace seastar;
using namespace std::chrono_literals;

// Fills the file with size bytes of random data, a megabyte at a time.
future<> fill(file& f, uint64_t size) {
    constexpr size_t chunk = 1 << 20;
    auto buf = temporary_buffer<char>::aligned(f.memory_dma_alignment(),
                                               chunk);
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

// snippet: cancel
future<> read_and_cancel(file& f, uint64_t size, int n) {
    io_intent intent;
    std::mt19937_64 random(1);
    constexpr size_t len = 1 << 20;
    std::uniform_int_distribution<uint64_t> block(0, size / len - 1);
    std::vector<future<temporary_buffer<char>>> reads;
    for (int i = 0; i < n; i++) {
        reads.push_back(f.dma_read<char>(block(random) * len, len, &intent));
    }
    co_await sleep(1ms);
    intent.cancel();
    int done = 0, cancelled = 0;
    for (auto& read : reads) {
        try {
            co_await std::move(read);
            done++;
        } catch (cancelled_error&) {
            cancelled++;
        }
    }
    fmt::print("{} reads: {} done, {} cancelled\n", n, done, cancelled);
}
// end snippet: cancel

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] -> future<> {
        uint64_t size = 256 << 20;
        auto f = co_await open_file_dma("cancel.dat", open_flags::rw |
            open_flags::create | open_flags::truncate);
        co_await fill(f, size);
        co_await read_and_cancel(f, size, 100);
        co_await f.close();
        co_await remove_file("cancel.dat");
    });
}
