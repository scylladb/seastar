// The solution of chapter 29's exercise 2: cycle.cc's session, without
// the cycle, in two ways.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/timer.hh>
#include <seastar/core/weak_ptr.hh>
#include <chrono>
#include <print>
#include <vector>

using namespace seastar;
using namespace std::chrono_literals;

// snippet: plain
// The timer is a member, so it's destroyed, and cancelled, with the
// session: its callback can't run after the session is gone, and needs
// no more than a plain pointer.
struct session {
    std::vector<char> buffer = std::vector<char>(1000);
    timer<> idle;
};

lw_shared_ptr<session> start_session() {
    auto s = make_lw_shared<session>();
    s->idle.set_callback([p = s.get()] {
        std::println("session idle for a minute: ending it ({} bytes)",
                     p->buffer.size());
    });
    s->idle.arm(1min);
    return s;
}
// end snippet: plain

// snippet: weak
// If the callback might outlive the session, it holds a weak_ptr, which
// doesn't keep the session alive, and becomes null when it's destroyed.
struct weak_session : public weakly_referencable<weak_session> {
    std::vector<char> buffer = std::vector<char>(1000);
    timer<> idle;
};

lw_shared_ptr<weak_session> start_weak_session() {
    auto s = make_lw_shared<weak_session>();
    s->idle.set_callback([w = s->weak_from_this()] {
        if (w) {
            std::println("session idle for a minute: ending it ({} bytes)",
                         w->buffer.size());
        }
    });
    s->idle.arm(1min);
    return s;
}
// end snippet: weak

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        auto s = start_session();
        auto w = start_weak_session();
        co_await sleep(10ms);
        s = nullptr;
        w = nullptr;
        std::println("main is done");
    });
}
