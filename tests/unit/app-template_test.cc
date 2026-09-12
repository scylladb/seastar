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
 * Copyright (C) 2023 ScyllaDB
 */

#define BOOST_TEST_MODULE app_template

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>
#include <boost/test/unit_test.hpp>
#include <seastar/core/app-template.hh>
#include <seastar/core/sleep.hh>

using namespace seastar;
using namespace std::chrono_literals;

// #2148 - always run this.
BOOST_AUTO_TEST_CASE(app_standard_memory_allocator) {
    // by default, use conservative settings instead of maxing out the performance
    // for testing app_template and underlying reactor's handling of different
    // settings
    app_template::seastar_options opts;
    opts.smp_opts.thread_affinity.set_value(false);
    opts.smp_opts.mbind.set_value(false);
    opts.smp_opts.smp.set_value(1);
    opts.smp_opts.lock_memory.set_value(false);
    opts.smp_opts.memory_allocator = memory_allocator::standard;
    opts.log_opts.default_log_level.set_value(log_level::error);
    app_template app{std::move(opts)};
    // app.run() takes `char**` not `char* const *`, so appease it
    std::string prog_name{"prog"};
    char* args[] = {prog_name.data()};
    int expected_status = 42;
    int actual_status = app.run(
        std::size(args), std::data(args),
        [expected_status] {
            // #2148 - add a small sleep to ensure the reactor does
            // some of its background stuff, pollers etc for example.
            // We only need to ensure we get put on the waiting task queue
            // to provoke the problem, thus a short (probably even to long here)
            // sleep will do.
            return seastar::sleep(2s).then([expected_status] {
                return make_ready_future<int>(expected_status);
            });
        });
    BOOST_CHECK_EQUAL(actual_status, expected_status);
}

BOOST_AUTO_TEST_CASE(return_0_for_func_returning_void) {
    app_template app;
    std::string prog_name{"prog"};
    char* args[] = {prog_name.data()};
    int status = app.run(std::size(args), std::data(args),
                         [] { return make_ready_future(); });
    BOOST_CHECK_EQUAL(status, 0);
}

BOOST_AUTO_TEST_CASE(return_status_for_func_returning_int) {
    app_template app;
    std::string prog_name{"prog"};
    char* args[] = {prog_name.data()};
    int expected_status = 42;
    int actual_status = app.run(
        std::size(args), std::data(args),
         [expected_status] {
             return make_ready_future<int>(expected_status);
         });
    BOOST_CHECK_EQUAL(actual_status, expected_status);
}

namespace {

// Saves an environment variable and restores it on destruction, so a test
// may set or unset it freely. The test binary itself may run under
// SEASTAR_OPTIONS (CI uses it to choose the reactor backend), so restoring
// matters beyond hygiene between cases.
class env_guard {
    std::string _name;
    std::optional<std::string> _saved;
public:
    explicit env_guard(std::string name) : _name(std::move(name)) {
        if (const char* v = std::getenv(_name.c_str())) {
            _saved = v;
        }
    }
    void set(const std::string& value) {
        ::setenv(_name.c_str(), value.c_str(), 1);
    }
    void unset() {
        ::unsetenv(_name.c_str());
    }
    ~env_guard() {
        if (_saved) {
            ::setenv(_name.c_str(), _saved->c_str(), 1);
        } else {
            ::unsetenv(_name.c_str());
        }
    }
};

// Points HOME at a fresh directory holding a .config/seastar/seastar.conf
// with the given content, so the default configuration reader sees exactly
// this file and nothing from the real home directory.
class tmp_home {
    env_guard _home{"HOME"};
    std::filesystem::path _dir;
public:
    explicit tmp_home(const std::string& conf) {
        std::string tmpl = (std::filesystem::temp_directory_path() / "app_template_test_XXXXXX").native();
        BOOST_REQUIRE(::mkdtemp(tmpl.data()) != nullptr);
        _dir = tmpl;
        std::filesystem::create_directories(_dir / ".config/seastar");
        std::ofstream{_dir / ".config/seastar/seastar.conf"} << conf;
        _home.set(_dir.native());
    }
    ~tmp_home() {
        std::error_code ec;
        std::filesystem::remove_all(_dir, ec);
    }
};

struct run_result {
    int status;
    bool ran = false;                    // whether the run function executed
    std::optional<std::string> app_opt;  // --app-opt, an option the app adds
    std::optional<unsigned> notify_ms;   // --blocked-reactor-notify-ms
};

// Runs an app which registers a string option --app-opt with the given
// command line, and reports the exit status and the values the app observes.
run_result run_app(std::vector<std::string> args) {
    // conservative settings, as in app_standard_memory_allocator above
    app_template::seastar_options opts;
    opts.smp_opts.thread_affinity.set_value(false);
    opts.smp_opts.mbind.set_value(false);
    opts.smp_opts.smp.set_value(1);
    opts.smp_opts.lock_memory.set_value(false);
    opts.smp_opts.memory_allocator = memory_allocator::standard;
    opts.log_opts.default_log_level.set_value(log_level::error);
    app_template app{std::move(opts)};
    app.add_options()
        ("app-opt", boost::program_options::value<std::string>(), "an application option");

    args.insert(args.begin(), "prog");
    std::vector<char*> argv;
    for (auto& arg : args) {
        argv.push_back(arg.data());
    }

    run_result r;
    r.status = app.run(argv.size(), argv.data(), [&] {
        r.ran = true;
        auto& conf = app.configuration();
        if (conf.count("app-opt")) {
            r.app_opt = conf["app-opt"].as<std::string>();
        }
        if (conf.count("blocked-reactor-notify-ms")) {
            r.notify_ms = conf["blocked-reactor-notify-ms"].as<unsigned>();
        }
        return make_ready_future<>();
    });
    return r;
}

} // namespace

BOOST_AUTO_TEST_CASE(env_options_set_an_option) {
    env_guard env{"SEASTAR_OPTIONS"};
    env.set("--app-opt from-env");
    auto r = run_app({});
    BOOST_REQUIRE(r.ran);
    BOOST_REQUIRE(r.app_opt.has_value());
    BOOST_CHECK_EQUAL(*r.app_opt, "from-env");
}

BOOST_AUTO_TEST_CASE(env_options_split_like_a_shell) {
    env_guard env{"SEASTAR_OPTIONS"};
    env.set("--app-opt \"two words\"");
    auto r = run_app({});
    BOOST_REQUIRE(r.ran);
    BOOST_REQUIRE(r.app_opt.has_value());
    BOOST_CHECK_EQUAL(*r.app_opt, "two words");
}

BOOST_AUTO_TEST_CASE(command_line_beats_env_options) {
    env_guard env{"SEASTAR_OPTIONS"};
    env.set("--app-opt from-env");
    auto r = run_app({"--app-opt", "from-cli"});
    BOOST_REQUIRE(r.ran);
    BOOST_REQUIRE(r.app_opt.has_value());
    BOOST_CHECK_EQUAL(*r.app_opt, "from-cli");
}

BOOST_AUTO_TEST_CASE(configuration_file_sets_an_option) {
    env_guard env{"SEASTAR_OPTIONS"};
    env.unset();
    tmp_home home{"blocked-reactor-notify-ms = 100\n"};
    auto r = run_app({});
    BOOST_REQUIRE(r.ran);
    BOOST_REQUIRE(r.notify_ms.has_value());
    BOOST_CHECK_EQUAL(*r.notify_ms, 100);
}

BOOST_AUTO_TEST_CASE(env_options_beat_the_configuration_file) {
    env_guard env{"SEASTAR_OPTIONS"};
    env.set("--blocked-reactor-notify-ms 200");
    tmp_home home{"blocked-reactor-notify-ms = 100\n"};
    auto r = run_app({});
    BOOST_REQUIRE(r.ran);
    BOOST_REQUIRE(r.notify_ms.has_value());
    BOOST_CHECK_EQUAL(*r.notify_ms, 200);
}

BOOST_AUTO_TEST_CASE(unknown_option_in_env_options_is_an_error) {
    env_guard env{"SEASTAR_OPTIONS"};
    env.set("--no-such-option");
    auto r = run_app({});
    BOOST_CHECK(!r.ran);
    BOOST_CHECK_EQUAL(r.status, 2);
}

BOOST_AUTO_TEST_CASE(positional_token_in_env_options_is_an_error) {
    env_guard env{"SEASTAR_OPTIONS"};
    env.set("--app-opt value stray-word");
    auto r = run_app({});
    BOOST_CHECK(!r.ran);
    BOOST_CHECK_EQUAL(r.status, 2);
}

BOOST_AUTO_TEST_CASE(duplicate_option_in_env_options_is_an_error) {
    env_guard env{"SEASTAR_OPTIONS"};
    env.set("--app-opt once --app-opt twice");
    auto r = run_app({});
    BOOST_CHECK(!r.ran);
    BOOST_CHECK_EQUAL(r.status, 2);
}

BOOST_AUTO_TEST_CASE(bad_escape_in_env_options_is_an_error) {
    env_guard env{"SEASTAR_OPTIONS"};
    env.set("--app-opt oops\\");
    auto r = run_app({});
    BOOST_CHECK(!r.ran);
    BOOST_CHECK_EQUAL(r.status, 2);
}
