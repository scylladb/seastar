// A background coroutine that is still waiting when the program exits.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/sleep.hh>
#include <chrono>
#include <print>
#include <vector>

using namespace seastar;
using namespace std::chrono_literals;

// snippet: abandon
future<> background() {
    std::vector<int> data(1000);
    co_await sleep(1h);
    std::println("{}", data.size());
}

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        (void)background();
        co_await sleep(10ms);
        std::println("main is done");
    });
}
// end snippet: abandon
