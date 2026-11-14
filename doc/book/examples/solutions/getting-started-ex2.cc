// Chapter 3, exercise 2: Shoal version 0 with a --greeting option, which
// every shard logs.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/smp.hh>
#include <seastar/util/log.hh>
#include <boost/program_options.hpp>
#include <cstdint>
#include <string>

using namespace seastar;
namespace bpo = boost::program_options;

// snippet: logger
logger shoal_log("shoal");
// end snippet: logger

// snippet: options
// Shoal's own settings, read from the command line.
struct options {
    uint16_t port;
    std::string greeting;
};
// end snippet: options

// snippet: shoal-main
future<> shoal_main(options opts) {
    shoal_log.info("Shoal starting on {} shards, port {}",
                   this_smp_shard_count(), opts.port);
    // opts lives in this coroutine's frame, which waits until every shard
    // is done, so the shards may use it by reference.
    co_await smp::invoke_on_all([&opts] {
        shoal_log.info("shard {} says: {}", this_shard_id(), opts.greeting);
    });
    shoal_log.info("Shoal version 0 can't serve anything yet; exiting");
}
// end snippet: shoal-main

// snippet: main
int main(int argc, char** argv) {
    app_template::config cfg;
    cfg.name = "Shoal";
    cfg.description = "Shoal, a Redis-compatible key-value server";
    app_template app(std::move(cfg));
    app.add_options()
        ("port", bpo::value<uint16_t>()->default_value(6379),
         "TCP port to listen on")
        ("greeting", bpo::value<std::string>()->default_value("hello"),
         "what every shard says when it starts");
    return app.run(argc, argv, [&app] {
        auto& config = app.configuration();
        return shoal_main(options{
            .port = config["port"].as<uint16_t>(),
            .greeting = config["greeting"].as<std::string>(),
        });
    });
}
// end snippet: main
