// A timeout that gives up waiting, and an abort that stops the operation.
#include <seastar/core/abort_on_expiry.hh>
#include <seastar/core/abort_source.hh>
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/lowres_clock.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/with_timeout.hh>
#include <chrono>
#include <print>

using namespace seastar;
using namespace std::chrono_literals;

// snippet: with-timeout
// An operation that takes 100 milliseconds.
future<int> slow_operation() {
    co_await sleep(100ms);
    std::println("slow_operation() finished");
    co_return 42;
}

future<> try_with_timeout() {
    try {
        int n = co_await with_timeout(lowres_clock::now() + 20ms,
                                      slow_operation());
        std::println("got {}", n);
    } catch (timed_out_error&) {
        std::println("timed out");
    }
}
// end snippet: with-timeout

// snippet: abort
// The same operation, which stops early if as is aborted.
future<int> abortable_operation(abort_source& as) {
    co_await sleep_abortable(100ms, as);
    std::println("abortable_operation() finished");
    co_return 42;
}

future<> try_with_abort() {
    abort_source as;
    auto sub = as.subscribe([] () noexcept {
        std::println("abort requested");
    });
    timer<> t([&as] { as.request_abort(); });
    t.arm(20ms);
    try {
        int n = co_await abortable_operation(as);
        std::println("got {}", n);
    } catch (sleep_aborted&) {
        std::println("aborted");
    }
}
// end snippet: abort

// snippet: abort-on-expiry
future<> try_with_deadline() {
    abort_on_expiry deadline(lowres_clock::now() + 20ms);
    try {
        int n = co_await abortable_operation(deadline.abort_source());
        std::println("got {}", n);
    } catch (std::exception& e) {
        std::println("stopped: {}", e.what());
    }
}
// end snippet: abort-on-expiry

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        co_await try_with_timeout();
        co_await sleep(150ms);
        std::println("--");
        co_await try_with_abort();
        co_await sleep(150ms);
        std::println("--");
        co_await try_with_deadline();
        co_await sleep(150ms);
    });
}
