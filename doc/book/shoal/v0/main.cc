// Shoal, version 0: the skeleton of a Seastar server. It reads its options,
// starts on every shard, logs what it found, and exits.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/smp.hh>
#include <seastar/util/log.hh>
#include <boost/program_options.hpp>
#include <cstdint>

using namespace seastar;
namespace bpo = boost::program_options;

// snippet: logger
logger shoal_log("shoal");
// end snippet: logger

// snippet: options
// Shoal's own settings, read from the command line.
struct options {
    uint16_t port;
};
// end snippet: options

// snippet: shoal-main
future<> shoal_main(options opts) {
    shoal_log.info("Shoal starting on {} shards, port {}",
                   this_smp_shard_count(), opts.port);
    co_await smp::invoke_on_all([] {
        shoal_log.debug("shard {} is ready", this_shard_id());
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
         "TCP port to listen on");
    return app.run(argc, argv, [&app] {
        auto& config = app.configuration();
        return shoal_main(options{
            .port = config["port"].as<uint16_t>(),
        });
    });
}
// end snippet: main
