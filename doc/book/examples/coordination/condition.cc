// Waiting for a condition with a condition_variable. A waiter that wakes
// up runs later, as a task, and by then another fiber may have changed
// the condition again.
#include <seastar/core/app-template.hh>
#include <seastar/core/condition-variable.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/util/later.hh>
#include <chrono>
#include <deque>
#include <print>

using namespace seastar;
using namespace std::chrono_literals;

// snippet: items
std::deque<int> items;
condition_variable item_added;

void add(int item) {
    items.push_back(item);
    item_added.signal();
}

// Waits until there is an item, and takes it.
future<int> take() {
    co_await item_added.wait([] { return !items.empty(); });
    int item = items.front();
    items.pop_front();
    co_return item;
}
// end snippet: items

// snippet: take-when
// The same, with when(), which checks the condition when the signal comes,
// and not again when the waiter runs.
future<int> take_when() {
    co_await item_added.when([] { return !items.empty(); });
    if (items.empty()) {
        co_return -1;   // woke up, and found no item
    }
    int item = items.front();
    items.pop_front();
    co_return item;
}
// end snippet: take-when

future<> remembered_signal() {
    // snippet: remembered
    item_added.signal();    // nobody waits, so the signal is remembered...
    co_await item_added.wait(100ms);    // ...and this returns at once
    std::println("wait() returned at once");
    // end snippet: remembered
}

future<> taken_meanwhile(future<int> (*take)(), const char* name) {
    // snippet: meanwhile
    auto waiting = take();
    add(1);         // wakes up the waiter, which runs later, as a task...
    int taken = items.front();      // ...but meanwhile, this code takes the item
    items.pop_front();
    co_await yield();               // let the waiter run
    if (waiting.available()) {
        std::println("{}: took {} first, and the waiter got {}", name, taken,
                     co_await std::move(waiting));
    } else {
        add(2);
        std::println("{}: took {} first, and the waiter waited for {}", name,
                     taken, co_await std::move(waiting));
    }
    // end snippet: meanwhile
}

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        co_await remembered_signal();
        co_await taken_meanwhile(take, "wait()");
        co_await taken_meanwhile(take_when, "when()");
    });
}
