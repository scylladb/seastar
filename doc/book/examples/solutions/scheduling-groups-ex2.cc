// Exercise 2 of "Scheduling groups": a fiber in the background group waits
// on a shared_future, which a timer made in the default group sets. Which
// group runs the timer's function, and which resumes the waiter?
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/scheduling.hh>
#include <seastar/core/shared_future.hh>
#include <seastar/core/timer.hh>
#include <seastar/core/with_scheduling_group.hh>
#include <chrono>
#include <print>
#include <string_view>

using namespace seastar;
using namespace std::chrono_literals;

void show(std::string_view where) {
    std::println("{:<36} {}", where, current_scheduling_group().name());
}

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        auto bg = co_await create_scheduling_group("background", 200);
        shared_promise<> ready;
        // Made here, in the default group.
        timer<> t([&ready] {
            show("the timer's function:");
            ready.set_value();
        });
        co_await with_scheduling_group(bg, [&] (this auto) -> future<> {
            show("the waiter, before it waits:");
            t.arm(1ms);
            co_await ready.get_shared_future();
            show("the waiter, after it resumed:");
        });
        co_await destroy_scheduling_group(bg);
    });
}
