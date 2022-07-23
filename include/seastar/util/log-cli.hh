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
 * Copyright (C) 2017 ScyllaDB
 */

#pragma once

#include <seastar/util/log.hh>
#include <seastar/util/program-options.hh>

#include <seastar/core/sstring.hh>

#include <boost/program_options.hpp>

#include <algorithm>
#include <unordered_map>

/// \addtogroup logging
/// @{
namespace seastar {

///
/// \brief Configure application logging at run-time with program options.
///
namespace log_cli {

///
/// \brief Options for controlling logging at run-time.
///
boost::program_options::options_description get_options_description();

using log_level_map = std::unordered_map<sstring, log_level>;

/// Logging configuration.
struct options : public program_options::option_group {
    /// \brief Default log level for log messages.
    ///
    /// Valid values are trace, debug, info, warn, error.
    /// Default: \p info
    /// \see \ref log_level.
    program_options::value<log_level> default_log_level;
    /// \brief Map of logger name to log level.
    ///
    /// The format is `NAME0=LEVEL0[:NAME1=LEVEL1:...]`.
    /// Valid logger names can be queried with \p --help-loggers.
    /// This option can be specified multiple times.
    program_options::value<log_level_map> logger_log_level;
    /// Select timestamp style for stdout logs.
    ///
    /// Default: \p real.
    /// \see logger_timestamp_style.
    program_options::value<logger_timestamp_style> logger_stdout_timestamps;
    /// \brief Send log output to output stream.
    ///
    /// As selected by \ref logger_ostream_type.
    /// Default: \p true.
    program_options::value<bool> log_to_stdout;
    /// Send log output to.
    ///
    /// Default: \p stderr.
    /// \see \ref seastar::logger_ostream_type.
    program_options::value<seastar::logger_ostream_type> logger_ostream_type;
    /// Send log output to syslog.
    ///
    /// Default: \p false.
    program_options::value<bool> log_to_syslog;

    /// Print colored tag prefix in log messages sent to output stream.
    ///
    /// Default: \p true.
    program_options::value<bool> log_with_color;
    /// \cond internal
    options(program_options::option_group* parent_group);
    /// \endcond
};

///
/// \brief Print a human-friendly list of the available loggers.
///
void print_available_loggers(std::ostream& os);

///
/// \brief Parse a log-level ({error, warn, info, debug, trace}) string, throwing \c std::runtime_error for an invalid
/// level.
///
log_level parse_log_level(const sstring&);

/// \cond internal
void parse_map_associations(const std::string& v, std::function<void(std::string, std::string)> consume_key_value);
/// \endcond

//
// \brief Parse associations from loggers to log-levels and write the resulting pairs to the output iterator.
//
// \throws \c std::runtime_error for an invalid log-level.
//
template <class OutputIter>
void parse_logger_levels(const program_options::string_map& levels, OutputIter out) {
    std::for_each(levels.begin(), levels.end(), [&out](auto&& pair) {
        *out++ = std::make_pair(pair.first, parse_log_level(pair.second));
    });
}

///
/// \brief Extract CLI options into a logging configuration.
//
logging_settings extract_settings(const boost::program_options::variables_map&);

///
/// \brief Extract \ref options into a logging configuration.
///
logging_settings extract_settings(const options&);

}

}

/// @}
