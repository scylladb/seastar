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
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 */

#include "app-template.hh"
#include "core/reactor.hh"
#include "core/scollectd.hh"
#include "core/metrics_api.hh"
#include <boost/program_options.hpp>
#include "core/print.hh"
#include "util/log.hh"
#include "util/log-cli.hh"
#include <boost/program_options.hpp>
#include <boost/make_shared.hpp>
#include <fstream>
#include <cstdlib>

namespace seastar {

namespace bpo = boost::program_options;

app_template::app_template(app_template::config cfg)
    : _cfg(std::move(cfg))
    , _opts(_cfg.name + " options")
    , _conf_reader(get_default_configuration_reader()) {
        _opts.add_options()
                ("help,h", "show help message")
                ;

        _opts_conf_file.add(reactor::get_options_description(cfg.default_task_quota));
        _opts_conf_file.add(seastar::metrics::get_options_description());
        _opts_conf_file.add(smp::get_options_description());
        _opts_conf_file.add(scollectd::get_options_description());
        _opts_conf_file.add(log_cli::get_options_description());

        _opts.add(_opts_conf_file);
}

app_template::configuration_reader app_template::get_default_configuration_reader() {
    return [this] (bpo::variables_map& configuration) {
        auto home = std::getenv("HOME");
        if (home) {
            std::ifstream ifs(std::string(home) + "/.config/seastar/seastar.conf");
            if (ifs) {
                bpo::store(bpo::parse_config_file(ifs, _opts_conf_file), configuration);
            }
            std::ifstream ifs_io(std::string(home) + "/.config/seastar/io.conf");
            if (ifs_io) {
                bpo::store(bpo::parse_config_file(ifs_io, _opts_conf_file), configuration);
            }
        }
    };
}

void app_template::set_configuration_reader(configuration_reader conf_reader) {
    _conf_reader = conf_reader;
}

boost::program_options::options_description& app_template::get_options_description() {
    return _opts;
}

boost::program_options::options_description& app_template::get_conf_file_options_description() {
    return _opts_conf_file;
}

boost::program_options::options_description_easy_init
app_template::add_options() {
    return _opts.add_options();
}

void
app_template::add_positional_options(std::initializer_list<positional_option> options) {
    for (auto&& o : options) {
        _opts.add(boost::make_shared<bpo::option_description>(o.name, o.value_semantic, o.help));
        _pos_opts.add(o.name, o.max_count);
    }
}


bpo::variables_map&
app_template::configuration() {
    return *_configuration;
}

int
app_template::run(int ac, char ** av, std::function<future<int> ()>&& func) {
    return run_deprecated(ac, av, [func = std::move(func)] () mutable {
        auto func_done = make_lw_shared<promise<>>();
        engine().at_exit([func_done] { return func_done->get_future(); });
        futurize_apply(func).finally([func_done] {
            func_done->set_value();
        }).then([] (int exit_code) {
            return engine().exit(exit_code);
        }).or_terminate();
    });
}

int
app_template::run(int ac, char ** av, std::function<future<> ()>&& func) {
    return run(ac, av, [func = std::move(func)] {
        return func().then([] () {
            return 0;
        });
    });
}

int
app_template::run_deprecated(int ac, char ** av, std::function<void ()>&& func) {
#ifdef SEASTAR_DEBUG
    print("WARNING: debug mode. Not for benchmarking or production\n");
#endif
    bpo::variables_map configuration;
    try {
        bpo::store(bpo::command_line_parser(ac, av)
                    .options(_opts)
                    .positional(_pos_opts)
                    .run()
            , configuration);
        _conf_reader(configuration);
    } catch (bpo::error& e) {
        print("error: %s\n\nTry --help.\n", e.what());
        return 2;
    }
    if (configuration.count("help")) {
        std::cout << _opts << "\n";
        return 1;
    }
    if (configuration["help-loggers"].as<bool>()) {
        log_cli::print_available_loggers(std::cout);
        return 1;
    }

    bpo::notify(configuration);

    // Needs to be before `smp::configure()`.
    try {
        apply_logging_settings(log_cli::extract_settings(configuration));
    } catch (const std::runtime_error& exn) {
        std::cout << "logging configuration error: " << exn.what() << '\n';
        return 1;
    }

    configuration.emplace("argv0", boost::program_options::variable_value(std::string(av[0]), false));
    try {
        smp::configure(configuration);
    } catch (...) {
        std::cerr << "Could not initialize seastar: " << std::current_exception() << std::endl;
        return 1;
    }
    _configuration = {std::move(configuration)};
    engine().when_started().then([this] {
        seastar::metrics::configure(this->configuration()).then([this] {
            // set scollectd use the metrics configuration, so the later
            // need to be set first
            scollectd::configure( this->configuration());
        });
    }).then(
        std::move(func)
    ).then_wrapped([] (auto&& f) {
        try {
            f.get();
        } catch (std::exception& ex) {
            std::cout << "program failed with uncaught exception: " << ex.what() << "\n";
            engine().exit(1);
        }
    });
    auto exit_code = engine().run();
    smp::cleanup();
    return exit_code;
}

}
