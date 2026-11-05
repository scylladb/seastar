// A gate keeps count of the work that runs in the background, so that its
// owner can wait for all of it, and refuse new work, before it goes away.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/gate.hh>
#include <seastar/core/sleep.hh>
#include <chrono>
#include <print>

using namespace seastar;
using namespace std::chrono_literals;

// snippet: gate
// Saves a file in the background. The holder, a parameter, lives in the
// coroutine's frame until the coroutine ends, and until then, the gate
// counts this work.
future<> save(int i, gate::holder) {
    co_await sleep(i * 10ms);
    std::println("saved file {}", i);
}

// Starts saving a file, and returns at once.
void start_saving(gate& g, int i) {
    (void)save(i, g.hold());    // hold() throws if the gate is closed
}

future<> run() {
    gate background;
    for (int i = 1; i <= 3; i++) {
        start_saving(background, i);
    }
    std::println("{} saves in the background", background.get_count());
    co_await background.close();    // waits until every holder is gone
    std::println("closed, {} in the background", background.get_count());
    try {
        start_saving(background, 4);
    } catch (gate_closed_exception& e) {
        std::println("too late: {}", e.what());
    }
}
// end snippet: gate

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, run);
}
