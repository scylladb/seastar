// Counting words with a sharded service: every word belongs to the shard
// that its hash picks, and that shard's instance of the service counts
// it. Shard 0 makes up the words, and sends each to its shard, one at a
// time (--variant one) or in batches (--variant batched).
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/sharded.hh>
#include <seastar/core/smp.hh>
#include <seastar/core/sstring.hh>
#include <seastar/coroutine/exception.hh>
#include <seastar/coroutine/parallel_for_each.hh>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <print>
#include <string>
#include <unordered_map>
#include <vector>

using namespace seastar;
namespace bpo = boost::program_options;

// snippet: service
// How many words a shard counted, and how many different ones.
struct counts {
    uint64_t words = 0;
    uint64_t distinct = 0;
    counts operator+(const counts& o) const {
        return {words + o.words, distinct + o.distinct};
    }
};

// The shard that a word belongs to.
unsigned owner(const sstring& word) {
    return std::hash<sstring>()(word) % this_smp_shard_count();
}

// One instance on every shard, which counts the words that belong to it.
class word_counter : public peering_sharded_service<word_counter> {
    std::unordered_map<sstring, uint64_t> _counts;
    uint64_t _words = 0;
public:
    // Counts a word that belongs to this shard.
    void add(const sstring& word) {
        _counts[word]++;
        _words++;
    }
    // Counts a word, on the shard that it belongs to.
    future<> count(sstring word) {
        return container().invoke_on(owner(word),
                [word] (word_counter& c) { c.add(word); });
    }
    counts get_counts() const {
        return {_words, _counts.size()};
    }
};
// end snippet: service

// The i-th word of the made-up text: one of 100,000 different words.
sstring make_word(uint64_t i) {
    return "word" + to_sstring(i * 2654435761u % 100'000);
}

// snippet: one
// Sends each word to its shard, and waits for the answer before sending
// the next one.
future<> count_one_at_a_time(sharded<word_counter>& counters, uint64_t n) {
    for (uint64_t i = 0; i < n; i++) {
        co_await counters.local().count(make_word(i));
    }
}
// end snippet: one

// snippet: batched
// Collects the words for each shard, and sends each shard its words in
// batches, all shards at once.
future<> count_batched(sharded<word_counter>& counters, uint64_t n) {
    constexpr uint64_t batch_words = 10'000;
    for (uint64_t start = 0; start < n; start += batch_words) {
        std::vector<std::vector<sstring>> batches(this_smp_shard_count());
        for (uint64_t i = start; i < std::min(n, start + batch_words); i++) {
            auto word = make_word(i);
            batches[owner(word)].push_back(std::move(word));
        }
        co_await coroutine::parallel_for_each(this_smp().all_shards(),
                [&] (unsigned shard) {
            return counters.invoke_on(shard,
                    [&batch = batches[shard]] (word_counter& c) {
                for (auto& word : batch) {
                    c.add(word);
                }
            });
        });
    }
}
// end snippet: batched

int main(int argc, char** argv) {
    app_template app;
    app.add_options()
        ("variant", bpo::value<std::string>()->default_value("batched"),
         "one or batched")
        ("words", bpo::value<uint64_t>()->default_value(1'000'000),
         "how many words to count");
    return app.run(argc, argv, [&app] (this auto) -> future<> {
        auto variant = app.configuration()["variant"].as<std::string>();
        auto n = app.configuration()["words"].as<uint64_t>();
        // snippet: lifecycle
        sharded<word_counter> counters;
        co_await counters.start();
        std::exception_ptr failure;
        try {
            auto start = std::chrono::steady_clock::now();
            if (variant == "one") {
                co_await count_one_at_a_time(counters, n);
            } else {
                co_await count_batched(counters, n);
            }
            std::chrono::duration<double> secs =
                std::chrono::steady_clock::now() - start;
            auto total = co_await counters.map_reduce0(
                [] (word_counter& c) { return c.get_counts(); },
                counts{}, std::plus<counts>());
            std::println("{} shards, {}: {} words, {} different, in "
                         "{:.0f} ns per word", this_smp_shard_count(),
                         variant, total.words, total.distinct,
                         secs.count() * 1e9 / n);
        } catch (...) {
            failure = std::current_exception();
        }
        // However the work ended, stop the service before it's destroyed.
        co_await counters.stop();
        if (failure) {
            co_await coroutine::return_exception_ptr(std::move(failure));
        }
        // end snippet: lifecycle
    });
}
