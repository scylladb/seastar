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
 * Copyright (C) 2015 Cloudius Systems, Ltd.
 */

#include <fmt/time.h>

#include <seastar/util/log.hh>
#include <seastar/util/log-cli.hh>

#include <seastar/core/array_map.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/print.hh>

#include <boost/any.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/range/adaptor/map.hpp>
#include <cxxabi.h>
#include <syslog.h>

#include <iostream>
#include <map>
#include <regex>
#include <string>
#include <system_error>
#include <chrono>

using namespace std::chrono_literals;

namespace seastar {

thread_local uint64_t logging_failures = 0;

void validate(boost::any& v,
              const std::vector<std::string>& values,
              logger_timestamp_style* target_type, int) {
    using namespace boost::program_options;
    validators::check_first_occurrence(v);
    auto s = validators::get_single_string(values);
    if (s == "none") {
        v = logger_timestamp_style::none;
        return;
    } else if (s == "boot") {
        v = logger_timestamp_style::boot;
        return;
    } else if (s == "real") {
        v = logger_timestamp_style::real;
        return;
    }
    throw validation_error(validation_error::invalid_option_value);
}

std::ostream& operator<<(std::ostream& os, logger_timestamp_style lts) {
    switch (lts) {
    case logger_timestamp_style::none: return os << "none";
    case logger_timestamp_style::boot: return os << "boot";
    case logger_timestamp_style::real: return os << "real";
    default: abort();
    }
    return os;
}

struct timestamp_tag {};

static void print_no_timestamp(std::ostream& os) {
}

static void print_space_and_boot_timestamp(std::ostream& os) {
    auto n = std::chrono::steady_clock::now().time_since_epoch() / 1us;
    fmt::print(os, " {:10d}.{:06d}", n / 1000000, n % 1000000);
}

static void print_space_and_real_timestamp(std::ostream& os) {
    struct a_second {
        time_t t;
        std::string s;
    };
    static thread_local a_second this_second;
    using clock = std::chrono::high_resolution_clock;
    auto n = clock::now();
    auto t = clock::to_time_t(n);
    if (this_second.t != t) {
        this_second.s = fmt::format("{:%Y-%m-%d %T}", fmt::localtime(t));
        this_second.t = t;
    }
    auto ms = (n - clock::from_time_t(t)) / 1ms;
    fmt::print(os, " {},{:03d}", this_second.s, ms);
}

static void (*print_timestamp)(std::ostream&) = print_no_timestamp;

static inline timestamp_tag space_and_current_timestamp() {
    return timestamp_tag{};
}

std::ostream& operator<<(std::ostream& os, timestamp_tag) {
    print_timestamp(os);
    return os;
}

const std::map<log_level, sstring> log_level_names = {
        { log_level::trace, "trace" },
        { log_level::debug, "debug" },
        { log_level::info, "info" },
        { log_level::warn, "warn" },
        { log_level::error, "error" },
};

std::ostream& operator<<(std::ostream& out, log_level level) {
    return out << log_level_names.at(level);
}

std::istream& operator>>(std::istream& in, log_level& level) {
    sstring s;
    in >> s;
    if (!in) {
        return in;
    }
    for (auto&& x : log_level_names) {
        if (s == x.second) {
            level = x.first;
            return in;
        }
    }
    in.setstate(std::ios::failbit);
    return in;
}

std::atomic<bool> logger::_stdout = { true };
std::atomic<bool> logger::_syslog = { false };

logger::logger(sstring name) : _name(std::move(name)) {
    global_logger_registry().register_logger(this);
}

logger::logger(logger&& x) : _name(std::move(x._name)), _level(x._level.load(std::memory_order_relaxed)) {
    global_logger_registry().moved(&x, this);
}

logger::~logger() {
    global_logger_registry().unregister_logger(this);
}

void
logger::really_do_log(log_level level, const char* fmt, const stringer* s, size_t n) {
    bool is_stdout_enabled = _stdout.load(std::memory_order_relaxed);
    bool is_syslog_enabled = _syslog.load(std::memory_order_relaxed);
    if(!is_stdout_enabled && !is_syslog_enabled) {
      return;
    }
    std::ostringstream out, log;
    static array_map<sstring, 20> level_map = {
            { int(log_level::debug), "DEBUG" },
            { int(log_level::info),  "INFO "  },
            { int(log_level::trace), "TRACE" },
            { int(log_level::warn),  "WARN "  },
            { int(log_level::error), "ERROR" },
    };
    auto print_once = [&] (std::ostream& out) {
      if (local_engine) {
        out << " [shard " << engine().cpu_id() << "] " << _name << " - ";
      } else {
        out << " " << _name << " - ";
      }
      const char* p = fmt;
      while (*p != '\0') {
        if (*p == '{' && *(p+1) == '}') {
            p += 2;
            if (n > 0) {
                try {
                    s->append(out, s->object);
                } catch (...) {
                    out << '<' << std::current_exception() << '>';
                }
                ++s;
                --n;
            } else {
                out << "???";
            }
        } else {
            out << *p++;
        }
      }
      out << "\n";
    };
    if (is_stdout_enabled) {
        out << level_map[int(level)] << space_and_current_timestamp();
        print_once(out);
        std::cout << out.str();
    }
    if (is_syslog_enabled) {
        print_once(log);
        static array_map<int, 20> level_map = {
                { int(log_level::debug), LOG_DEBUG },
                { int(log_level::info), LOG_INFO },
                { int(log_level::trace), LOG_DEBUG },  // no LOG_TRACE
                { int(log_level::warn), LOG_WARNING },
                { int(log_level::error), LOG_ERR },
        };
        // NOTE: syslog() can block, which will stall the reactor thread.
        //       this should be rare (will have to fill the pipe buffer
        //       before syslogd can clear it) but can happen.  If it does,
        //       we'll have to implement some internal buffering (which
        //       still means the problem can happen, just less frequently).
        // syslog() interprets % characters, so send msg as a parameter
        auto msg = log.str();
        syslog(level_map[int(level)], "%s", msg.c_str());
    }
}

void logger::failed_to_log(std::exception_ptr ex)
{
    try {
        do_log(log_level::error, "failed to log message: {}", ex);
    } catch (...) {
        ++logging_failures;
    }
}

void
logger::set_stdout_enabled(bool enabled) {
    _stdout.store(enabled, std::memory_order_relaxed);
}

void
logger::set_syslog_enabled(bool enabled) {
    _syslog.store(enabled, std::memory_order_relaxed);
}

bool logger::is_shard_zero() {
    return engine().cpu_id() == 0;
}

void
logger_registry::set_all_loggers_level(log_level level) {
    std::lock_guard<std::mutex> g(_mutex);
    for (auto&& l : _loggers | boost::adaptors::map_values) {
        l->set_level(level);
    }
}

log_level
logger_registry::get_logger_level(sstring name) const {
    std::lock_guard<std::mutex> g(_mutex);
    return _loggers.at(name)->level();
}

void
logger_registry::set_logger_level(sstring name, log_level level) {
    std::lock_guard<std::mutex> g(_mutex);
    _loggers.at(name)->set_level(level);
}

std::vector<sstring>
logger_registry::get_all_logger_names() {
    std::lock_guard<std::mutex> g(_mutex);
    auto ret = _loggers | boost::adaptors::map_keys;
    return std::vector<sstring>(ret.begin(), ret.end());
}

void
logger_registry::register_logger(logger* l) {
    std::lock_guard<std::mutex> g(_mutex);
    if (_loggers.find(l->name()) != _loggers.end()) {
        throw std::runtime_error(format("Logger '{}' registered twice", l->name()));
    }
    _loggers[l->name()] = l;
}

void
logger_registry::unregister_logger(logger* l) {
    std::lock_guard<std::mutex> g(_mutex);
    _loggers.erase(l->name());
}

void
logger_registry::moved(logger* from, logger* to) {
    std::lock_guard<std::mutex> g(_mutex);
    _loggers[from->name()] = to;
}

void apply_logging_settings(const logging_settings& s) {
    global_logger_registry().set_all_loggers_level(s.default_level);

    for (const auto& pair : s.logger_levels) {
        try {
            global_logger_registry().set_logger_level(pair.first, pair.second);
        } catch (const std::out_of_range&) {
            throw std::runtime_error(
                        seastar::format("Unknown logger '{}'. Use --help-loggers to list available loggers.",
                                        pair.first));
        }
    }

    logger::set_stdout_enabled(s.stdout_enabled);
    logger::set_syslog_enabled(s.syslog_enabled);

    switch (s.stdout_timestamp_style) {
    case logger_timestamp_style::none:
        print_timestamp = print_no_timestamp;
        break;
    case logger_timestamp_style::boot:
        print_timestamp = print_space_and_boot_timestamp;
        break;
    case logger_timestamp_style::real:
        print_timestamp = print_space_and_real_timestamp;
        break;
    default:
        break;
    }
}

sstring pretty_type_name(const std::type_info& ti) {
    int status;
    std::unique_ptr<char[], void (*)(void*)> result(
            abi::__cxa_demangle(ti.name(), 0, 0, &status), std::free);
    return result.get() ? result.get() : ti.name();
}

logger_registry& global_logger_registry() {
    static logger_registry g_registry;
    return g_registry;
}

sstring level_name(log_level level) {
    return  log_level_names.at(level);
}

namespace log_cli {

namespace bpo = boost::program_options;

log_level parse_log_level(const sstring& s) {
    try {
        return boost::lexical_cast<log_level>(s.c_str());
    } catch (const boost::bad_lexical_cast&) {
        throw std::runtime_error(format("Unknown log level '{}'", s));
    }
}

bpo::options_description get_options_description() {
    bpo::options_description opts("Logging options");

    opts.add_options()
            ("default-log-level",
             bpo::value<sstring>()->default_value("info"),
             "Default log level for log messages. Valid values are trace, debug, info, warn, error."
            )
            ("logger-log-level",
             bpo::value<program_options::string_map>()->default_value({}),
             "Map of logger name to log level. The format is \"NAME0=LEVEL0[:NAME1=LEVEL1:...]\". "
             "Valid logger names can be queried with --help-logging. "
             "Valid values for levels are trace, debug, info, warn, error. "
             "This option can be specified multiple times."
            )
            ("logger-stdout-timestamps", bpo::value<logger_timestamp_style>()->default_value(logger_timestamp_style::real),
                    "Select timestamp style for stdout logs: none|boot|real")
            ("log-to-stdout", bpo::value<bool>()->default_value(true), "Send log output to stdout.")
            ("log-to-syslog", bpo::value<bool>()->default_value(false), "Send log output to syslog.")
            ("help-loggers", bpo::bool_switch(), "Print a list of logger names and exit.");

    return opts;
}

void print_available_loggers(std::ostream& os) {
    auto names = global_logger_registry().get_all_logger_names();
    // For quick searching by humans.
    std::sort(names.begin(), names.end());

    os << "Available loggers:\n";

    for (auto&& name : names) {
        os << "    " << name << '\n';
    }
}

logging_settings extract_settings(const boost::program_options::variables_map& vars) {
    const auto& raw_levels = vars["logger-log-level"].as<program_options::string_map>();

    std::unordered_map<sstring, log_level> levels;
    parse_logger_levels(raw_levels, std::inserter(levels, levels.begin()));

    return logging_settings{
        std::move(levels),
        parse_log_level(vars["default-log-level"].as<sstring>()),
        vars["log-to-stdout"].as<bool>(),
        vars["log-to-syslog"].as<bool>(),
        vars["logger-stdout-timestamps"].as<logger_timestamp_style>()
    };

}

}

}
namespace boost {
template<>
seastar::log_level lexical_cast(const std::string& source) {
    std::istringstream in(source);
    seastar::log_level level;
    if (!(in >> level)) {
        throw boost::bad_lexical_cast();
    }
    return level;
}

}

namespace std {
std::ostream& operator<<(std::ostream& out, const std::exception_ptr& eptr) {
    if (!eptr) {
        out << "<no exception>";
        return out;
    }
    try {
        std::rethrow_exception(eptr);
    } catch(...) {
        auto tp = abi::__cxa_current_exception_type();
        if (tp) {
            out << seastar::pretty_type_name(*tp);
        } else {
            // This case shouldn't happen...
            out << "<unknown exception>";
        }
        // Print more information on some familiar exception types
        try {
            throw;
        } catch(const std::system_error &e) {
            out << " (error " << e.code() << ", " << e.what() << ")";
        } catch(const std::exception& e) {
            out << " (" << e.what() << ")";
            try {
                std::rethrow_if_nested(e);
            } catch (...) {
                out << ": " << std::current_exception();
            }
        } catch(...) {
            // no extra info
        }
    }
    return out;
}

std::ostream& operator<<(std::ostream& out, const std::exception& e) {
    return out << seastar::pretty_type_name(typeid(e)) << " (" << e.what() << ")";
}

std::ostream& operator<<(std::ostream& out, const std::system_error& e) {
    return out << seastar::pretty_type_name(typeid(e)) << " (error " << e.code() << ", " << e.what() << ")";
}

}
