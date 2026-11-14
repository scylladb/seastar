// Chapter 21, exercise 5: how processes end. Runs a program that exits
// with a status, one that is killed by a signal, and one that doesn't
// exist. Run it with one shard: -c1.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/seastar.hh>
#include <seastar/util/process.hh>
#include <exception>
#include <format>
#include <print>
#include <string>
#include <variant>

using namespace seastar;

// snippet: run
// Runs a program, and returns how it ended.
future<std::string> run(std::string path, std::vector<sstring> argv) {
    try {
        auto p = co_await spawn_process(path, {.argv = std::move(argv)});
        auto status = co_await p.wait();
        if (auto* e = std::get_if<process::wait_exited>(&status)) {
            co_return std::format("exited with status {}", e->exit_code);
        }
        auto& s = std::get<process::wait_signaled>(status);
        co_return std::format("killed by signal {}", s.terminating_signal);
    } catch (std::exception& e) {
        co_return std::format("failed to start: {}", e.what());
    }
}
// end snippet: run

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        for (auto command : {"exit 3", "kill -9 $$"}) {
            auto how = co_await run("/bin/sh", {"sh", "-c", command});
            std::println("{}", how);
        }
        std::println("{}", co_await run("/no/such/program", {"program"}));
    });
}
