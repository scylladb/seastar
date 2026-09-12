/*
 * This file is open source software, licensed to you under the terms
 * of the Apache License, Version 2.0 (the "License").  See the NOTICE file
 * distributed with this work for additional information regarding copyright
 * ownership.  You may not use this file except in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
#include <random>
#include <chrono>
#include <seastar/core/app-template.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/sleep.hh>
#include <seastar/util/log.hh>
#include <seastar/http/httpd.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/prometheus.hh>
#include <seastar/util/defer.hh>
#include <seastar/core/metrics.hh>
#include <seastar/core/metrics_api.hh>
#include <seastar/core/metrics_registration.hh>
#include <fmt/format.h>

using namespace seastar;
using namespace std::chrono_literals;
namespace sm = seastar::metrics;
namespace bpo = boost::program_options;

logger applog("app");

class metrics_example {
private:
    sm::metric_groups _metrics;
    uint64_t _counter;
    uint64_t _triggered;

public:
    metrics_example() : _counter(0) {
        // Note that these metrics are only available as long at the _metrics instance
        // is valid.
        _metrics.add_group("metrics_demo", {
            // Access the member each time the metric is read.
            // Note that counter is expected to be monotonically increasing.
            sm::make_counter("counter", _counter, sm::description("A counter that counts.")),
            // A function that can return any integer
            sm::make_counter("function", [this]() {applog.info("triggered"); return ++_triggered;}, sm::description("A function which returns a value.")),
            // A gauge is a counter that can go up and down, can be used for rates
            sm::make_gauge("gauge", []() {return 3;}, sm::description("arbitrary number"))
        });
    }

    void increase(uint64_t val = 1) { _counter += val; }
};

class prometheus_example {
private:
    httpd::http_server_control _server;
    bool _was_started;
public:
    prometheus_example(sstring address, uint16_t port, sstring prefix) {
        prometheus::config pctx;
        net::inet_address prom_addr(address);
        pctx.metric_help = "seastar::httpd server statistics";

        std::cout << "starting prometheus API server" << std::endl;
        _server.start("prometheus").get();
        prometheus::start(_server, pctx).get();
        _was_started = true;

        _server.listen(socket_address{prom_addr, port}).handle_exception([prom_addr, port] (auto ep) {
            std::cerr << seastar::format("Could not start Prometheus API server on {}:{}: {}\n", prom_addr, port, ep);
            return make_exception_future<>(ep);
        }).get();
    }

    ~prometheus_example() {
        if (_was_started) {
            std::cout << "Stoppping Prometheus server" << std::endl;  // This can throw, but won't.
            _server.stop().get();
        }
    }
};

/**
 * In order to access the metrics we need to use one of the builtin mechanisms.
 * For the sake of this example we are using prometheus.
 * To view the specific metrics of the demo use something in the lines of
 *  curl -s http://localhost:9180/metrics | grep "metrics_demo"
 *
 * # HELP seastar_metrics_demo_counter A counter that counts.
 * # TYPE seastar_metrics_demo_counter counter
 * seastar_metrics_demo_counter{shard="0"} 90
 * # HELP seastar_metrics_demo_function A function which returns a value.
 * # TYPE seastar_metrics_demo_function counter
 * seastar_metrics_demo_function{shard="0"} 139884580384626
 * # HELP seastar_metrics_demo_gauge arbitrary number
 * # TYPE seastar_metrics_demo_gauge gauge
 * seastar_metrics_demo_gauge{shard="0"} 3.000000
 * 
 */
int main(int argc, char** argv) {
    seastar::app_template app;
    app.add_options()("prometheus_port", bpo::value<uint16_t>()->default_value(9180), "Prometheus port. Set to zero in order to disable.");
    app.add_options()("prometheus_address", bpo::value<sstring>()->default_value("0.0.0.0"), "Prometheus address");
    app.add_options()("prometheus_prefix", bpo::value<sstring>()->default_value("seastar_httpd"), "Prometheus metrics prefix");

    return app.run(argc, argv, [&] {
        return seastar::async([&] {
            auto&& config = app.configuration();
            uint16_t pport = config["prometheus_port"].as<uint16_t>();
            sstring address = config["prometheus_address"].as<sstring>();
            sstring prefix = config["prometheus_prefix"].as<sstring>();
            prometheus_example server(address, pport, prefix);

            applog.info("Now try `curl http://127.0.0.1:{}/metrics` to see how metrics are exported", pport);
            metrics_example metrics;
            for (;;) {
                sleep(1s).get();
                metrics.increase(5);
            }

            return 0;
        });
    });
}
