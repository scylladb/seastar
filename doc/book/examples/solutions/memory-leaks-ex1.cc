// Chapter 29, exercise 1: abandon.cc, stopping cleanly: the background
// coroutine holds a gate, and an abort source ends its sleep.
#include <seastar/core/abort_source.hh>
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/gate.hh>
#include <seastar/core/sleep.hh>
#include <chrono>
#include <print>
#include <vector>

using namespace seastar;
using namespace std::chrono_literals;

future<> background(abort_source& stopping, gate::holder) {
    std::vector<int> data(1000);
    try {
        co_await sleep_abortable(1h, stopping);
    } catch (sleep_aborted&) {
        co_return;
    }
    std::println("{}", data.size());
}

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        abort_source stopping;
        gate background_work;
        (void)background(stopping, background_work.hold());
        co_await sleep(10ms);
        std::println("main is done");
        stopping.request_abort();
        co_await background_work.close();
    });
}
