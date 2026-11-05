// What exceptions cost a server under overload: clients whose requests
// time out while waiting to be admitted, and retry at once. With
// exceptions, every timeout costs about a microsecond of CPU, which the
// server needs for its real work.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/semaphore.hh>
#include <seastar/core/sleep.hh>
#include <seastar/coroutine/as_future.hh>
#include <chrono>
#include <cstdint>
#include <print>
#include <vector>

using namespace seastar;
using namespace std::chrono_literals;

struct counts {
    uint64_t served = 0;
    uint64_t timed_out = 0;
    bool done = false;
};

// The work of serving a request: 10 microseconds of CPU.
void serve() {
    auto end = std::chrono::steady_clock::now() + 10us;
    while (std::chrono::steady_clock::now() < end) {
    }
}

// snippet: with-exceptions
future<> client_with_exceptions(semaphore& admission, counts& c) {
    while (!c.done) {
        try {
            auto units = co_await get_units(admission, 1, 1ms);
            serve();
            c.served++;
        } catch (semaphore_timed_out&) {
            c.timed_out++;
        }
    }
}
// end snippet: with-exceptions

// snippet: without-exceptions
future<> client_without_exceptions(semaphore& admission, counts& c) {
    while (!c.done) {
        auto f = co_await coroutine::as_future(get_units(admission, 1, 1ms));
        if (f.failed()) {
            f.ignore_ready_future();
            c.timed_out++;
            continue;
        }
        auto units = f.get();
        serve();
        c.served++;
    }
}
// end snippet: without-exceptions

template <typename Client>
future<> run(const char* name, int clients, Client client) {
    semaphore admission(1);
    counts c;
    std::vector<future<>> running;
    for (int i = 0; i < clients; i++) {
        running.push_back(client(admission, c));
    }
    co_await sleep(2s);
    c.done = true;
    for (auto& f : running) {
        co_await std::move(f);
    }
    std::println("{:5} clients, {:18}: {:7} served/s, {:8} timed out/s",
        clients, name, c.served / 2, c.timed_out / 2);
}

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        for (int clients : {50, 100, 200, 500, 1000, 2000}) {
            co_await run("with exceptions", clients, client_with_exceptions);
            co_await run("without exceptions", clients, client_without_exceptions);
        }
    });
}
