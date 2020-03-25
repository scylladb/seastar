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

///
/// \brief Print a human-friendly list of the available loggers.
///
void print_available_loggers(std::ostream& os);

///
/// \brief Parse a log-level ({error, warn, info, debug, trace}) string, throwing \c std::runtime_error for an invalid
/// level.
///
log_level parse_log_level(const sstring&);

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

}

}

/// @}
