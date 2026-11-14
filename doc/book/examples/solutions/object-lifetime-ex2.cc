// Chapter 8, exercise 2: a lambda coroutine given to app_template::run(),
// which captures app by reference, and uses it after its first co_await.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/sleep.hh>
#include <chrono>
#include <print>

using namespace seastar;
using namespace std::chrono_literals;

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [&app] () -> future<> {
        co_await sleep(10ms);
        auto smp = app.configuration()["smp"].as<unsigned>();
        std::println("smp count: {}", smp);
    });
}
