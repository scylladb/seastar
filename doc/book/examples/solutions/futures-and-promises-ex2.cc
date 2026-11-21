// Chapter 6, exercise 2: shared.cc's loader, with a load that fails the
// first time. With --variant broken, the failed load stays in the map;
// with --variant fixed, the loader forgets it, and the next request tries
// again.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/shared_future.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/sstring.hh>
#include <seastar/coroutine/as_future.hh>
#include <fmt/format.h>
#include <chrono>
#include <print>
#include <stdexcept>
#include <string>
#include <unordered_map>

using namespace seastar;
using namespace std::chrono_literals;
namespace bpo = boost::program_options;

class loader {
    std::unordered_map<sstring, shared_future<sstring>> _values;
    int _loads = 0;
    bool _fixed;

    // Fails the first time it's called, and succeeds after that.
    future<sstring> load(sstring key) {
        ++_loads;
        co_await sleep(100ms);
        if (_loads == 1) {
            throw std::runtime_error("the disk is busy");
        }
        co_return fmt::format("the value of {}", key);
    }

    // Loads the key, and if the load fails, forgets it, so that the next
    // request for the key loads it again.
    future<sstring> load_or_forget(sstring key) {
        auto f = co_await coroutine::as_future(load(key));
        if (f.failed()) {
            _values.erase(key);
        }
        co_return co_await std::move(f);
    }
public:
    explicit loader(bool fixed) : _fixed(fixed) {}
    future<sstring> get(const sstring& key) {
        auto it = _values.find(key);
        if (it == _values.end()) {
            auto f = _fixed ? load_or_forget(key) : load(key);
            it = _values.emplace(key, std::move(f)).first;
        }
        return it->second.get_future();
    }
    int loads() const {
        return _loads;
    }
};

future<> ask(loader& l, int who, sstring key) {
    try {
        auto value = co_await l.get(key);
        std::println("{} got {}", who, value);
    } catch (std::runtime_error& e) {
        std::println("{} failed: {}", who, e.what());
    }
}

int main(int argc, char** argv) {
    app_template app;
    app.add_options()
        ("variant", bpo::value<std::string>()->default_value("broken"),
         "broken or fixed");
    return app.run(argc, argv, [&app] () -> future<> {
        auto variant = app.configuration()["variant"].as<std::string>();
        loader l(variant == "fixed");
        for (int who = 1; who <= 3; who++) {
            co_await ask(l, who, "x");
        }
        std::println("{} loads", l.loads());
    });
}
