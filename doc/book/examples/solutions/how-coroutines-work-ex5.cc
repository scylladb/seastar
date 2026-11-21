// Chapter 24, exercise 5: a std::generator that yields the keys of a map
// that start with a prefix, and a coroutine that takes them, yielding to
// the reactor between keys. Run it with one shard: -c1.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/coroutine/maybe_yield.hh>
#include <generator>
#include <map>
#include <print>
#include <string>
#include <string_view>

using namespace seastar;

// snippet: prefix
// The keys of m that start with prefix, in order. A std::generator can't
// co_await, so it can't yield to the reactor itself.
std::generator<const std::string&> keys_with_prefix(
        const std::map<std::string, int>& m, std::string prefix) {
    for (auto it = m.lower_bound(prefix);
         it != m.end() && it->first.starts_with(prefix); ++it) {
        co_yield it->first;
    }
}

// Counts the keys with the prefix, letting other tasks run between them.
future<int> count_keys(const std::map<std::string, int>& m,
                       std::string prefix) {
    int n = 0;
    for ([[maybe_unused]] const std::string& key :
            keys_with_prefix(m, prefix)) {
        n++;
        co_await coroutine::maybe_yield();
    }
    co_return n;
}
// end snippet: prefix

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        std::map<std::string, int> m;
        for (int i = 0; i < 100'000; i++) {
            m[std::format("{}:{}", i % 3 ? "user" : "item", i)] = i;
        }
        std::println("{} keys start with \"item:\"",
                     co_await count_keys(m, "item:"));
    });
}
