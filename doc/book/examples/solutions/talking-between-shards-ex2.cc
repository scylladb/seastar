// Chapter 14, exercise 2: wordcount.cc's batched counting, with every
// shard making its share of the words, the words whose numbers it gets
// when the numbers are dealt out round-robin, and sending its batches to
// their shards.
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
#include <unordered_map>
#include <vector>

using namespace seastar;
namespace bpo = boost::program_options;

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
    void add(const sstring& word) {
        _counts[word]++;
        _words++;
    }
    counts get_counts() const {
        return {_words, _counts.size()};
    }
    // Makes this shard's share of the first n words, and sends them to
    // their shards, in batches.
    future<> count_my_share(uint64_t n);
};

// The i-th word of the made-up text: one of 100,000 different words.
sstring make_word(uint64_t i) {
    return "word" + to_sstring(i * 2654435761u % 100'000);
}

future<> word_counter::count_my_share(uint64_t n) {
    constexpr uint64_t batch_words = 10'000;
    unsigned shards = this_smp_shard_count();
    // This shard's words are this_shard_id(), and every shards-th after.
    for (uint64_t start = this_shard_id(); start < n;
         start += batch_words * shards) {
        std::vector<std::vector<sstring>> batches(shards);
        uint64_t end = std::min(n, start + batch_words * shards);
        for (uint64_t i = start; i < end; i += shards) {
            auto word = make_word(i);
            batches[owner(word)].push_back(std::move(word));
        }
        co_await coroutine::parallel_for_each(this_smp().all_shards(),
                [&] (unsigned shard) {
            return container().invoke_on(shard,
                    [&batch = batches[shard]] (word_counter& c) {
                for (auto& word : batch) {
                    c.add(word);
                }
            });
        });
    }
}

int main(int argc, char** argv) {
    app_template app;
    app.add_options()
        ("words", bpo::value<uint64_t>()->default_value(1'000'000),
         "how many words to count");
    return app.run(argc, argv, [&app] (this auto) -> future<> {
        auto n = app.configuration()["words"].as<uint64_t>();
        sharded<word_counter> counters;
        co_await counters.start();
        std::exception_ptr failure;
        try {
            auto start = std::chrono::steady_clock::now();
            co_await counters.invoke_on_all([n] (word_counter& c) {
                return c.count_my_share(n);
            });
            std::chrono::duration<double> secs =
                std::chrono::steady_clock::now() - start;
            auto total = co_await counters.map_reduce0(
                [] (word_counter& c) { return c.get_counts(); },
                counts{}, std::plus<counts>());
            std::println("{} shards, every shard makes words: {} words, "
                         "{} different, in {:.0f} ns per word",
                         this_smp_shard_count(), total.words,
                         total.distinct, secs.count() * 1e9 / n);
        } catch (...) {
            failure = std::current_exception();
        }
        co_await counters.stop();
        if (failure) {
            co_await coroutine::return_exception_ptr(std::move(failure));
        }
    });
}
