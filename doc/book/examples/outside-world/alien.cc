// A thread that isn't Seastar's talks to the shards: it asks each shard
// for a number with alien::submit_to(), and waits for the answers, as
// std::futures. Run it with two shards: -c2.
#include <seastar/core/alien.hh>
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/future.hh>
#include <seastar/core/smp.hh>
#include <future>
#include <print>
#include <thread>
#include <vector>

using namespace seastar;

// snippet: outside
// Runs in a thread of its own, which Seastar doesn't know: asks every
// shard for its id, times a hundred, and waits for the answers.
int ask_shards(alien::instance& seastar, unsigned shards) {
    std::vector<std::future<int>> answers;
    for (shard_id shard = 0; shard < shards; shard++) {
        answers.push_back(alien::submit_to(seastar, shard, [] {
            // This runs on the shard, so it may use Seastar as usual,
            // and must return a future.
            return make_ready_future<int>(this_shard_id() * 100);
        }));
    }
    int sum = 0;
    for (auto& answer : answers) {
        sum += answer.get();        // blocks this thread, not a shard
    }
    return sum;
}
// end snippet: outside

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [&app] () -> future<> {
        // snippet: start
        // Start the outside thread, and wait for its answer without
        // blocking the shard: the thread's last act is to send the answer
        // here with alien::run_on(), which fulfills a promise on this
        // shard. What the thread needs from Seastar is found here, on the
        // shard: the thread can call nothing of Seastar's but the alien
        // functions.
        alien::instance& seastar = app.alien();
        unsigned shards = this_smp_shard_count();
        shard_id home = this_shard_id();
        promise<int> answer;
        std::thread outside([&] {
            int sum = ask_shards(seastar, shards);
            // sum by value: run_on() returns before the shard runs this.
            alien::run_on(seastar, home, [&answer, sum] () noexcept {
                answer.set_value(sum);
            });
        });
        int sum = co_await answer.get_future();
        outside.join();             // it's about to end, so this hardly waits
        std::println("the shards' answers add up to {}", sum);
        // end snippet: start
    });
}
