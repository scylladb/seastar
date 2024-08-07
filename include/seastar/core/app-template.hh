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

#ifndef SEASTAR_MODULE
#include <boost/program_options.hpp>
#include <functional>
#include <chrono>
#endif
#include <seastar/core/future.hh>
#include <seastar/core/smp.hh>
#include <seastar/core/smp_options.hh>
#include <seastar/core/sstring.hh>
#include <seastar/util/program-options.hh>
#include <seastar/core/metrics_api.hh>
#include <seastar/core/scollectd.hh>
#include <seastar/util/log-cli.hh>
#include <seastar/util/modules.hh>

namespace seastar {

namespace alien {

class instance;

}

SEASTAR_MODULE_EXPORT
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
        /// via seastar::handle_signal(signo, handler, once); instead.
        bool auto_handle_sigint_sigterm = true;
        /// Specifies the default value for linux-aio I/O control blocks. This translates
        /// to the maximum number of sockets the shard can handle.
        unsigned max_networking_aio_io_control_blocks = 10000;
        /// The amount of memory that should not be used by the seastar allocator,
        /// additional to the amount of memory already reserved for the OS.
        /// This can be used when the application allocates some of its memory using the
        /// seastar allocator, and some using the system allocator, in particular when it
        /// uses the mmap system call with MAP_ANONYMOUS which is not overridden in seastar.
        size_t reserve_additional_memory_per_shard = 0;
        config() {}
    };

    /// Seastar configuration options
    struct seastar_options : public program_options::option_group {
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
        /// \brief Handle SIGINT/SIGTERM by calling reactor::stop()
        ///
        /// When true, Seastar will set up signal handlers for SIGINT/SIGTERM that call
        /// reactor::stop(). The reactor will then execute callbacks installed by
        /// reactor::at_exit().
        ///
        /// When false, Seastar will not set up signal handlers for SIGINT/SIGTERM
        /// automatically. The default behavior (terminate the program) will be kept.
        /// You can adjust the behavior of SIGINT/SIGTERM by installing signal handlers
        /// via seastar::handle_signal(signo, handler, once); instead.
        bool auto_handle_sigint_sigterm = true;
        /// Configuration options for the reactor.
        reactor_options reactor_opts;
        /// Configuration for the metrics sub-system.
        metrics::options metrics_opts;
        /// Configuration options for the smp aspect of seastar.
        smp_options smp_opts;
        /// Configuration for the scollectd sub-system.
        scollectd::options scollectd_opts;
        /// Configuration for the logging sub-system.
        log_cli::options log_opts;

        seastar_options();
    };

    using configuration_reader = std::function<void (boost::program_options::variables_map&)>;
private:
    // unique_ptr to avoid pulling in alien.hh.
    std::unique_ptr<alien::instance> _alien;
    // reactor destruction is asynchronous, so we must let the last reactor
    // destroy the smp instance
    std::shared_ptr<smp> _smp;
    seastar_options _opts;
    boost::program_options::options_description _app_opts;
    boost::program_options::options_description _seastar_opts;
    boost::program_options::options_description _opts_conf_file;
    boost::program_options::positional_options_description _pos_opts;
    std::optional<boost::program_options::variables_map> _configuration;
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
    explicit app_template(seastar_options opts);
    explicit app_template(config cfg = config());
    ~app_template();

    const seastar_options& options() const;

    boost::program_options::options_description& get_options_description();
    boost::program_options::options_description& get_conf_file_options_description();
    boost::program_options::options_description_easy_init add_options();
    void add_positional_options(std::initializer_list<positional_option> options);
    boost::program_options::variables_map& configuration();
    int run_deprecated(int ac, char ** av, std::function<void ()>&& func) noexcept;

    void set_configuration_reader(configuration_reader conf_reader);

    /// Obtains an alien::instance object that can be used to send messages
    /// to Seastar shards from non-Seastar threads.
    alien::instance& alien() { return *_alien; }

    // Runs given function and terminates the application when the future it
    // returns resolves. The value with which the future resolves will be
    // returned by this function.
    int run(int ac, char ** av, std::function<future<int> ()>&& func) noexcept;

    // Like run() which takes std::function<future<int>()>, but returns
    // with exit code 0 when the future returned by func resolves
    // successfully.
    int run(int ac, char ** av, std::function<future<> ()>&& func) noexcept;
};

}
