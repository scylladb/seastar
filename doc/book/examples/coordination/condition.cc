// Waiting for a condition with a condition_variable. A signal that finds
// nobody waiting is remembered, for the next wait. A waiter that wakes up
// runs later, as a task, and by then another fiber may have changed the
// condition again.
#include <seastar/core/app-template.hh>
#include <seastar/core/condition-variable.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/sleep.hh>
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
    while (items.empty()) {
        co_await item_added.wait();
    }
    int item = items.front();
    items.pop_front();
    co_return item;
}
// end snippet: items

// snippet: take-once
// Wrong: waits once, and doesn't check again when it runs.
future<int> take_once() {
    co_await item_added.wait();
    if (items.empty()) {
        co_return -1;   // woke up, and found no item
    }
    int item = items.front();
    items.pop_front();
    co_return item;
}
// end snippet: take-once

// With wait() and a predicate, which checks the predicate after the waiter
// wakes up, in a task of its own.
future<int> take_predicate() {
    co_await item_added.wait([] { return !items.empty(); });
    if (items.empty()) {
        co_return -1;
    }
    int item = items.front();
    items.pop_front();
    co_return item;
}

// snippet: take-when
// With when(), which checks the predicate when the signal comes, inside
// signal(), and not again when the waiter runs.
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

// snippet: remembered
int version = 0;        // the state: each change counts up
int saved = 0;          // the version that the last save wrote
int saves = 0;
condition_variable changed;

void change() {
    ++version;
    changed.signal();
}

// Saves the state after each change, in the background, until
// changed.broken() stops it.
future<> saver() {
    for (;;) {
        co_await changed.wait();
        int v = version;
        co_await sleep(100ms);      // writing the state takes a while
        saved = v;
        ++saves;
    }
}
// end snippet: remembered

future<> remembered_signal() {
    auto running = saver();
    change();                   // wakes up the saver, which saves version 1
    co_await sleep(10ms);
    change();                   // two changes during that save, when
    change();                   // nobody waits for a signal
    co_await sleep(300ms);
    std::println("version {}, saved {}, in {} saves", version, saved, saves);
    changed.broken();
    try {
        co_await std::move(running);
    } catch (broken_condition_variable&) {
    }
}

future<> race_once() {
    // snippet: meanwhile
    auto waiting = take_once();
    add(1);                     // wakes up the waiter, which runs later...
    int taken = items.front();  // ...but meanwhile, this code takes the item
    items.pop_front();
    co_await yield();           // let the waiter run
    add(2);                     // for a waiter that is still waiting
    std::println("took {} first, and the waiter got {}", taken,
                 co_await std::move(waiting));
    // end snippet: meanwhile
}

// The same as race_once(), with another version of take().
future<> race(future<int> (*take_item)()) {
    auto waiting = take_item();
    add(1);                     // wakes up the waiter, which runs later...
    int taken = items.front();  // ...but meanwhile, this code takes the item
    items.pop_front();
    co_await yield();           // let the waiter run
    add(2);                     // for a waiter that is still waiting
    std::println("took {} first, and the waiter got {}", taken,
                 co_await std::move(waiting));
}

// After race(): if the waiter didn't take item 2, takes it, and the signal
// that came with it, which found nobody waiting and was remembered, so
// that the next race() starts clean.
future<> clean_up() {
    if (!items.empty()) {
        items.clear();
        co_await item_added.wait();     // returns at once
    }
}

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        co_await remembered_signal();
        co_await race_once();
        co_await clean_up();
        co_await race(take);
        co_await clean_up();
        co_await race(take_predicate);
        co_await clean_up();
        co_await race(take_when);
        co_await clean_up();
    });
}
