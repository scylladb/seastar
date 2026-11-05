// Chapter 10, exercise 4: gate.cc, with the gate destroyed, without
// close(), while a save in the background still holds it.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/gate.hh>
#include <seastar/core/sleep.hh>
#include <chrono>
#include <print>

using namespace seastar;
using namespace std::chrono_literals;

// Saves a file in the background. The holder, a parameter, lives in the
// coroutine's frame until the coroutine ends, and until then, the gate
// counts this work.
future<> save(int i, gate::holder) {
    co_await sleep(i * 10ms);
    std::println("saved file {}", i);
}

future<> run() {
    {
        gate background;
        (void)save(1, background.hold());
        std::println("destroying the gate, without close()");
    }
    co_await sleep(50ms);
    std::println("done");
}

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, run);
}
