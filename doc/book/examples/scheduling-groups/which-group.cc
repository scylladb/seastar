// Which scheduling group runs a piece of code? This program prints the
// current group in each of the situations that decide it: a coroutine and
// the continuations it creates, a message to another shard, a timer, a
// fiber woken by another group, a seastar::thread, and switching groups.
// Run it with two shards: --smp 2.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/scheduling.hh>
#include <seastar/core/semaphore.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/smp.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/timer.hh>
#include <seastar/core/with_scheduling_group.hh>
#include <seastar/coroutine/switch_to.hh>
#include <chrono>
#include <print>
#include <string_view>

using namespace seastar;
using namespace std::chrono_literals;

void show(std::string_view where) {
    std::println("{:<44} {}", where, current_scheduling_group().name());
}

// snippet: inherit
// Runs in whichever group calls it, and so does all that it does next.
future<> background_work() {
    show("a coroutine, called in background:");
    co_await sleep(1ms);
    show("the same coroutine, after a co_await:");
    co_await sleep(1ms).then([] {
        show("a continuation it created:");
    });
    co_await smp::submit_to(1, [] {
        show("its message, run on shard 1:");
    });
}
// end snippet: inherit

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        auto bg = co_await create_scheduling_group("background", 200);
        show("main(), run by app_template:");
        co_await with_scheduling_group(bg, background_work);

        // snippet: timer
        // A timer runs its function in the group that was current when
        // the timer was made, wherever it's armed.
        timer<> made_in_main([] { show("a timer made in main:"); });
        timer<> made_for_bg(bg, [] {
            show("a timer made for background:");
        });
        co_await with_scheduling_group(bg, [&] {
            made_in_main.arm(1ms);
            made_for_bg.arm(1ms);
            return sleep(5ms);
        });
        // end snippet: timer

        // snippet: wake
        // A fiber that waits resumes in its own group, whichever group
        // wakes it.
        semaphore sem(0);
        auto waiter = sem.wait().then([] {
            show("a waiter in main, woken by background:");
        });
        co_await with_scheduling_group(bg, [&] {
            sem.signal();
            return make_ready_future<>();
        });
        co_await std::move(waiter);
        // end snippet: wake

        // snippet: switch
        // A seastar::thread runs in the group that its attributes give;
        // a thread, and a coroutine, can switch groups as they run.
        thread_attributes attr;
        attr.sched_group = bg;
        co_await async(attr, [main = current_scheduling_group()] {
            show("a thread started for background:");
            thread::switch_to(main);
            show("the thread, after switch_to(main):");
        });
        co_await coroutine::switch_to(bg);
        show("main(), after coroutine::switch_to(bg):");
        // end snippet: switch
        co_await coroutine::switch_to(default_scheduling_group());
        co_await destroy_scheduling_group(bg);
    });
}
