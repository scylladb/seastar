// Chapter 8, exercise 2: a lambda coroutine given to app_template::run(),
// with "this auto", which moves the lambda into the coroutine's frame.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/sleep.hh>
#include <chrono>
#include <print>

using namespace seastar;
using namespace std::chrono_literals;

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [&app] (this auto) -> future<> {
        co_await sleep(10ms);
        auto smp = app.configuration()["smp"].as<unsigned>();
        std::println("smp count: {}", smp);
    });
}
