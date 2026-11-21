// Running another program: spawns sort(1), writes lines to its standard
// input, reads the sorted lines from its standard output, and waits for it
// to end. Run it with one shard: -c1.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/iostream.hh>
#include <seastar/core/seastar.hh>
#include <seastar/util/process.hh>
#include <print>
#include <string>
#include <variant>

using namespace seastar;

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        // snippet: spawn
        auto sort = co_await spawn_process("/usr/bin/sort",
            spawn_parameters{.argv = {"sort", "--reverse"}});
        // Write its input, and close it, so that it knows that the input
        // has ended.
        auto in = sort.cin();
        for (auto word : {"pear\n", "apple\n", "fig\n"}) {
            co_await in.write(word);
        }
        co_await in.close();
        // Read its output, to the end.
        auto out = sort.cout();
        std::string sorted;
        while (auto buf = co_await out.read()) {
            sorted.append(buf.get(), buf.size());
        }
        co_await out.close();
        // And wait for it to end, which also reaps the process.
        auto status = co_await sort.wait();
        auto* exited = std::get_if<process::wait_exited>(&status);
        std::print("{}sort exited with status {}\n", sorted,
                   exited ? exited->exit_code : -1);
        // end snippet: spawn
    });
}
