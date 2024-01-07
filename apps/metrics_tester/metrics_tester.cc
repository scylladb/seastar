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
/*
 * Copyright (C) 2024 ScyllaDB
 */

#include <seastar/core/app-template.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/prometheus.hh>
#include <seastar/core/metrics.hh>
#include <seastar/core/relabel_config.hh>
#include <seastar/core/internal/estimated_histogram.hh>
#include "../lib/stop_signal.hh"
using namespace seastar;
using namespace std::chrono_literals;

struct serializer {};

int main(int ac, char** av) {
    namespace bpo = boost::program_options;

    app_template app;
    auto opt_add = app.add_options();
    opt_add
        ("listen", bpo::value<sstring>()->default_value("0.0.0.0"), "address to start Prometheus server on")
        ("port", bpo::value<uint16_t>()->default_value(9180), "Prometheus port")
    ;
    httpd::http_server_control prometheus_server;

    return app.run(ac, av, [&] {
        return seastar::async([&] {
            namespace sm = seastar::metrics;
            sm::internal::time_estimated_histogram histogram;
            sm::metric_groups _metrics;
            histogram.add_micro(1000);
            histogram.add_micro(2000);
            histogram.add_micro(30000);
            seastar_apps_lib::stop_signal stop_signal;
            auto& opts = app.configuration();
            auto& listen = opts["listen"].as<sstring>();
            auto private_label = sm::label_instance("private", "1");
            auto& port = opts["port"].as<uint16_t>();
            _metrics.add_group("test_group", {
                    sm::make_gauge("gauge1", [] { return 1; },
                            sm::description("A test gauge"),{private_label} ),
                    sm::make_histogram("histogram1", [histogram]{return histogram.to_metrics_histogram();}
                           ,sm::description("A test histogram"),{private_label})
            });
            smp::invoke_on_all([] {
                    std::vector<metrics::relabel_config> rl(2);
                    rl[0].source_labels = {"__name__"};
                    rl[0].expr = ".*";
                    rl[0].action = metrics::relabel_config::relabel_action::drop;

                    rl[1].source_labels = {"private"};
                    rl[1].expr = "1";
                    rl[1].action = metrics::relabel_config::relabel_action::keep;
                    return metrics::set_relabel_configs(rl).then([](metrics::metric_relabeling_result) {
                        return;
                    });
            }).get();

            if (port) {
                prometheus_server.start("prometheus").get();

                prometheus::config pctx;
                pctx.allow_protobuf = true;
                prometheus::start(prometheus_server, pctx).get();
                prometheus_server.listen(socket_address{listen, port}).handle_exception([] (auto ep) {
                    return make_exception_future<>(ep);
                }).get();
                stop_signal.wait().get();
            }
        });
    });
}
