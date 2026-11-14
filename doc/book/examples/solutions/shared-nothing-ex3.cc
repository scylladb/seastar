// Chapter 13, exercise 3: shard 0 copies an lw_shared_ptr into the capture
// of a lambda that shard 1 runs. With --variant read, shard 1 only reads
// through it; with --variant copy, shard 1 also copies it.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/core/smp.hh>
#include <print>
#include <string>
#include <vector>

using namespace seastar;
namespace bpo = boost::program_options;

int main(int argc, char** argv) {
    app_template app;
    app.add_options()
        ("variant", bpo::value<std::string>()->default_value("read"),
         "read or copy");
    return app.run(argc, argv, [&app] () -> future<> {
        if (this_smp_shard_count() < 2) {
            throw std::runtime_error("run with at least two shards, -c2");
        }
        auto variant = app.configuration()["variant"].as<std::string>();
        auto data = make_lw_shared<std::vector<int>>(1000);
        // snippet: capture
        size_t size;
        if (variant == "read") {
            // Shard 1 only reads the vector through the pointer.
            size = co_await smp::submit_to(1, [data] {
                return data->size();
            });
        } else {
            // Shard 1 copies the pointer, and destroys the copy.
            size = co_await smp::submit_to(1, [data] {
                auto copy = data;
                return copy->size();
            });
        }
        std::println("shard 1 saw {} elements; use count {}, should be 1",
                     size, data.use_count());
        // end snippet: capture
    });
}
