// Chapter 8, exercise 4: does futurize_invoke() keep a lambda coroutine's
// captures alive? --variant statement awaits its future in the same
// statement; --variant later keeps the future, and awaits it later.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/future.hh>
#include <seastar/core/sleep.hh>
#include <chrono>
#include <print>
#include <string>

using namespace seastar;
using namespace std::chrono_literals;
namespace bpo = boost::program_options;

// Returns a lambda coroutine that captures a string, longer than fits in
// the string object itself, and uses it after waiting.
auto make_lambda() {
    return [s = std::string(100, 'x')] () -> future<size_t> {
        co_await sleep(10ms);
        co_return s.size();
    };
}

future<size_t> in_one_statement() {
    co_return co_await futurize_invoke(make_lambda());
}

future<size_t> awaited_later() {
    auto f = futurize_invoke(make_lambda());
    co_return co_await std::move(f);
}

int main(int argc, char** argv) {
    app_template app;
    app.add_options()
        ("variant", bpo::value<std::string>()->default_value("statement"),
         "statement or later");
    return app.run(argc, argv, [&app] (this auto) -> future<> {
        auto variant = app.configuration()["variant"].as<std::string>();
        size_t size = co_await (variant == "later" ? awaited_later()
                                                   : in_one_statement());
        std::println("{}: the string has {} characters", variant, size);
    });
}
