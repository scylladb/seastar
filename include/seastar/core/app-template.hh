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
#pragma once

#include <boost/program_options.hpp>
#include <boost/optional.hpp>
#include <functional>
#include <seastar/core/future.hh>
#include <seastar/core/sstring.hh>
#include <chrono>

namespace seastar {

class app_template {
public:
    struct config {
        /// The name of the application.
        ///
        /// Will be used in the --help output to distinguish command line args
        /// registered by the application, as opposed to those registered by
        /// seastar and its subsystems.
        sstring name = "App";
        /// The description of the application.
        ///
        /// Will be printed on the top of the --help output. Lines should be
        /// hard-wrapped for 80 chars.
        sstring description = "";
        std::chrono::duration<double> default_task_quota = std::chrono::microseconds(500);
        /// \brief Handle SIGINT/SIGTERM by calling reactor::stop()
        ///
        /// When true, Seastar will set up signal handlers for SIGINT/SIGTERM that call
        /// reactor::stop(). The reactor will then execute callbacks installed by
        /// reactor::at_exit().
        ///
        /// When false, Seastar will not set up signal handlers for SIGINT/SIGTERM
        /// automatically. The default behavior (terminate the program) will be kept.
        /// You can adjust the behavior of SIGINT/SIGTERM by installing signal handlers
        /// via reactor::handle_signal().
        bool auto_handle_sigint_sigterm = true;
        config() {}
    };

    using configuration_reader = std::function<void (boost::program_options::variables_map&)>;
private:
    config _cfg;
    boost::program_options::options_description _opts;
    boost::program_options::options_description _opts_conf_file;
    boost::program_options::positional_options_description _pos_opts;
    boost::optional<boost::program_options::variables_map> _configuration;
    configuration_reader _conf_reader;

    configuration_reader get_default_configuration_reader();
public:
    struct positional_option {
        const char* name;
        const boost::program_options::value_semantic* value_semantic;
        const char* help;
        int max_count;
    };
public:
    explicit app_template(config cfg = config());

    boost::program_options::options_description& get_options_description();
    boost::program_options::options_description& get_conf_file_options_description();
    boost::program_options::options_description_easy_init add_options();
    void add_positional_options(std::initializer_list<positional_option> options);
    boost::program_options::variables_map& configuration();
    int run_deprecated(int ac, char ** av, std::function<void ()>&& func);

    void set_configuration_reader(configuration_reader conf_reader);

    // Runs given function and terminates the application when the future it
    // returns resolves. The value with which the future resolves will be
    // returned by this function.
    int run(int ac, char ** av, std::function<future<int> ()>&& func);

    // Like run() which takes std::function<future<int>()>, but returns
    // with exit code 0 when the future returned by func resolves
    // successfully.
    int run(int ac, char ** av, std::function<future<> ()>&& func);
};

}
