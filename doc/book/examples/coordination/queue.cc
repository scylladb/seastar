// A queue between a fast producer and a slow consumer. When the queue is
// full, the producer waits, so it goes at the consumer's pace, and the
// items waiting never use more memory than the queue allows.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/queue.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/when_all.hh>
#include <algorithm>
#include <chrono>
#include <optional>
#include <print>

using namespace seastar;
using namespace std::chrono_literals;

// snippet: queue
// Produces ten items, as fast as the queue lets it, and then an empty
// item, which says that there are no more.
future<> produce(queue<std::optional<int>>& q) {
    for (int i = 1; i <= 10; i++) {
        co_await q.push_eventually(i);      // waits while q is full
    }
    co_await q.push_eventually(std::nullopt);
}

// Consumes items slowly, until the empty item.
future<> consume(queue<std::optional<int>>& q) {
    size_t most_waiting = 0;
    while (auto item = co_await q.pop_eventually()) {
        most_waiting = std::max(most_waiting, q.size() + 1);
        co_await sleep(1ms);    // the work on the item
        std::println("consumed {}", *item);
    }
    std::println("no more items; at most {} waited", most_waiting);
}
// end snippet: queue

future<> run() {
    queue<std::optional<int>> q(3);     // at most three items wait
    co_await when_all(produce(q), consume(q));
}

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, run);
}
