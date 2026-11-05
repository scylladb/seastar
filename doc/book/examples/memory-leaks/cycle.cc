// An object that keeps itself alive: its timer's callback holds an
// lw_shared_ptr to it.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/timer.hh>
#include <chrono>
#include <print>
#include <vector>

using namespace seastar;
using namespace std::chrono_literals;

// snippet: cycle
// A client's session, which ends itself if the client is idle for a
// minute. The timer's callback must keep the session alive, in case it
// fires after everyone else let go of it, so it holds a pointer to it.
struct session {
    std::vector<char> buffer = std::vector<char>(1000);
    timer<> idle;
};

lw_shared_ptr<session> start_session() {
    auto s = make_lw_shared<session>();
    s->idle.set_callback([s] {
        std::println("session idle for a minute: ending it");
    });
    s->idle.arm(1min);
    return s;
}

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        auto s = start_session();
        co_await sleep(10ms);
        // The client left: cancel the timer, and drop the session.
        s->idle.cancel();
        s = nullptr;
        std::println("main is done");
    });
}
// end snippet: cycle
