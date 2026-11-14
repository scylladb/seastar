// Chapter 2, exercise 1: hello, with a long sleep, so that its threads can
// be listed while it runs.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/sleep.hh>
#include <chrono>
#include <print>

using namespace seastar;
namespace bpo = boost::program_options;

int main(int argc, char** argv) {
    app_template app;
    app.add_options()
        ("seconds", bpo::value<int>()->default_value(30),
         "how long to sleep");
    return app.run(argc, argv, [&app] () -> future<> {
        // snippet: sleep
        std::println("Hello, world");
        auto seconds = app.configuration()["seconds"].as<int>();
        co_await sleep(std::chrono::seconds(seconds));
        // end snippet: sleep
    });
}
