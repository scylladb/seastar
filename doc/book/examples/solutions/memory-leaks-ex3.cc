// The solution of chapter 29's exercise 3: stuck.cc, whose requests give
// up on a lost reply after a second.
#include <seastar/core/app-template.hh>
#include <seastar/core/condition-variable.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/gate.hh>
#include <seastar/core/memory.hh>
#include <seastar/core/sleep.hh>
#include <print>
#include <unordered_set>
#include <vector>

using namespace seastar;

// Each request waits for its reply, on a condition variable that the code
// that receives replies signals. One reply in a hundred is lost, and its
// request gives up after a second.
struct server {
    condition_variable replies;
    std::unordered_set<int> answered;
    gate requests;

    future<> request(int id) {
        auto holder = requests.hold();
        std::vector<char> buffer(1000);     // the request's own memory
        try {
            co_await replies.wait(std::chrono::seconds(1),
                                  [this, id] { return answered.contains(id); });
            answered.erase(id);
        } catch (condition_variable_timed_out&) {
            // The reply was lost: give up on it.
        }
    }
    void reply(int id) {
        answered.insert(id);
        replies.broadcast();
    }
    // Wakes the requests still waiting, with an exception, and waits for
    // them to end.
    future<> stop() {
        replies.broken();
        return requests.close();
    }
};

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        server s;
        int id = 0;
        for (int round = 1; round <= 40; round++) {
            for (int i = 0; i < 10000; i++, id++) {
                (void)s.request(id).handle_exception([] (auto) {});
                if (id % 100 != 0) {
                    s.reply(id);
                }
            }
            // Let the requests that got their replies end, before measuring.
            co_await sleep(std::chrono::milliseconds(100));
            if (round % 5 != 0) {
                continue;
            }
            auto stats = memory::stats();
            std::println("after round {}: {} KB allocated, {} live objects",
                         round, stats.allocated_memory() / 1024,
                         stats.live_objects());
        }
        co_await s.stop();
    });
}
