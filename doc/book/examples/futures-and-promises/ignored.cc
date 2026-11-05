// Futures that nobody waits for: the work goes on, and a failure that
// nobody sees is reported in the log.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/sleep.hh>
#include <chrono>
#include <print>
#include <stdexcept>

using namespace seastar;
using namespace std::chrono_literals;

// snippet: fail-later
future<> fail_later() {
    co_await sleep(10ms);
    std::println("fail_later() is still running");
    throw std::runtime_error("nobody is listening");
}
// end snippet: fail-later

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        // snippet: main
        (void)fail_later();
        std::println("dropped the future");
        co_await sleep(50ms);
        std::println("done");
        // end snippet: main
    });
}
