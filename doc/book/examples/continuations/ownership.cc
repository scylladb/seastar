// Who owns the objects that continuations use: a capture that owns a
// moved object, and do_with(), which keeps an object alive for a whole
// chain that uses it by reference. Run it with one shard: -c1.
#include <seastar/core/app-template.hh>
#include <seastar/core/do_with.hh>
#include <seastar/core/sleep.hh>
#include <chrono>
#include <numeric>
#include <print>
#include <vector>

using namespace seastar;
using namespace std::chrono_literals;

// snippet: move
// Returns the sum of v's elements, after 10 milliseconds.
future<int> slow_sum(std::vector<int> v) {
    return sleep(10ms).then([v = std::move(v)] {
        return std::accumulate(v.begin(), v.end(), 0);
    });
}
// end snippet: move

// snippet: by-reference
// Appends 1 and then 2 to v, each after 10 milliseconds. The caller must
// keep v alive until the returned future is resolved.
future<> slow_append_twice(std::vector<int>& v) {
    return sleep(10ms).then([&v] {
        v.push_back(1);
        return sleep(10ms);
    }).then([&v] {
        v.push_back(2);
    });
}

// Appends twice to a new vector, and returns its size.
future<size_t> appended_size() {
    return do_with(std::vector<int>(), [] (std::vector<int>& v) {
        return slow_append_twice(v).then([&v] {
            return v.size();
        });
    });
}
// end snippet: by-reference

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] {
        return slow_sum({1, 2, 3}).then([] (int sum) {
            std::println("slow_sum: {}", sum);
            return appended_size();
        }).then([] (size_t size) {
            std::println("appended_size: {}", size);
        });
    });
}
