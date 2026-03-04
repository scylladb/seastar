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
 * Copyright (C) 2025 Cloudius Systems, Ltd.
 */
#pragma once


#include <iosfwd>
#include <fmt/core.h>

/// \addtogroup logging
/// @{

namespace seastar {


/// \brief log level used with \see {logger}
/// used with the logger.do_log method.
/// Levels are in increasing order. That is if you want to see debug(3) logs you
/// will also see error(0), warn(1), info(2).
///
enum class log_level {
    error,
    warn,
    info,
    debug,
    trace,
};

std::ostream& operator<<(std::ostream& out, log_level level);
std::istream& operator>>(std::istream& in, log_level& level);

}

template <>
struct fmt::formatter<seastar::log_level> {
    constexpr auto parse(format_parse_context& ctx) { return ctx.begin(); }
    auto format(seastar::log_level level, fmt::format_context& ctx) const -> decltype(ctx.out());
};

/// @}
