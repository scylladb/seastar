// An asynchronous function that throws instead of returning a failed
// future, what that does to a caller that checks each future, and the
// two ways to fix it.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/future-util.hh>
#include <seastar/core/sleep.hh>
#include <seastar/coroutine/as_future.hh>
#include <chrono>
#include <print>
#include <stdexcept>
#include <string>

using namespace seastar;
using namespace std::chrono_literals;

future<> write_key(std::string key) {
    co_await sleep(1ms);
    std::println("stored \"{}\"", key);
}

// snippet: store
// Not a coroutine: it checks the key, and returns write_key()'s future.
future<> store(std::string key) {
    if (key.empty()) {
        throw std::invalid_argument("empty key");
    }
    return write_key(std::move(key));
}
// end snippet: store

// snippet: store-all-bad
// Stores each key, reports the ones it can't store, and goes on.
future<> store_all_bad() {
    for (std::string key : {"a", "", "c"}) {
        auto f = co_await coroutine::as_future(store(key));
        if (f.failed()) {
            std::println("couldn't store \"{}\"", key);
            f.ignore_ready_future();
        }
    }
}
// end snippet: store-all-bad

// snippet: store-carefully
future<> store_carefully(std::string key) {
    if (key.empty()) {
        return make_exception_future<>(std::invalid_argument("empty key"));
    }
    return write_key(std::move(key));
}
// end snippet: store-carefully

// snippet: store-all-good
future<> store_all_good() {
    for (std::string key : {"a", "", "c"}) {
        auto f = co_await coroutine::as_future(futurize_invoke(store, key));
        if (f.failed()) {
            std::println("couldn't store \"{}\"", key);
            f.ignore_ready_future();
        }
    }
}
// end snippet: store-all-good

// Not shown in the book: store_all_bad()'s loop, calling store_carefully().
// The book says that it works.
future<> store_all_carefully() {
    for (std::string key : {"a", "", "c"}) {
        auto f = co_await coroutine::as_future(store_carefully(key));
        if (f.failed()) {
            std::println("couldn't store \"{}\"", key);
            f.ignore_ready_future();
        }
    }
}

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        // snippet: main
        std::println("with store_all_bad():");
        try {
            co_await store_all_bad();
        } catch (std::exception& e) {
            std::println("store_all_bad() threw: {}", e.what());
        }
        std::println("with store_all_good():");
        co_await store_all_good();
        // end snippet: main
        std::println("with store_carefully():");
        co_await store_all_carefully();
    });
}
