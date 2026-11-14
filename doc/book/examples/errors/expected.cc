// Reporting an expected failure as a value, with std::expected, instead of
// as an exception.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/sstring.hh>
#include <seastar/util/later.hh>
#include <charconv>
#include <cstdint>
#include <expected>
#include <map>
#include <print>

using namespace seastar;

// snippet: get-number
enum class lookup_error { not_found, not_a_number };

// Gives the value of a key as a number, or says why it can't.
future<std::expected<int64_t, lookup_error>> get_number(
        const std::map<sstring, sstring>& data, sstring key) {
    co_await yield();    // standing in for a wait, for example for a disk
    auto it = data.find(key);
    if (it == data.end()) {
        co_return std::unexpected(lookup_error::not_found);
    }
    int64_t n;
    auto [end, ec] = std::from_chars(it->second.begin(), it->second.end(), n);
    if (ec != std::errc() || end != it->second.end()) {
        co_return std::unexpected(lookup_error::not_a_number);
    }
    co_return n;
}
// end snippet: get-number

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        std::map<sstring, sstring> data{{"x", "42"}, {"y", "hello"}};
        // snippet: main
        for (const char* key : {"x", "y", "z"}) {
            auto n = co_await get_number(data, key);
            if (n) {
                std::println("{} is {}", key, *n);
            } else if (n.error() == lookup_error::not_found) {
                std::println("{} wasn't found", key);
            } else {
                std::println("{} isn't a number", key);
            }
        }
        // end snippet: main
    });
}
