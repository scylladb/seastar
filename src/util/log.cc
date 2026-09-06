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


#include <iostream>
#include <map>
#include <memory>
#include <regex>
#include <string>
#include <string_view>
#include <system_error>
#include <chrono>
#include <algorithm>
#include <ranges>

#include <fmt/core.h>
#include <fmt/chrono.h>
#include <fmt/color.h>
#include <fmt/ostream.h>
#include <fmt/std.h>
#include <boost/any.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/program_options.hpp>
#include <cxxabi.h>
#include <syslog.h>
#include <unistd.h>
#include <paths.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <cerrno>
#include <cstring>


#include <seastar/util/log.hh>
#include <seastar/util/log-cli.hh>

#include <seastar/util/internal/array_map.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/future.hh>


#include "core/program_options.hh"

using namespace std::chrono_literals;

struct wrapped_log_level {
    seastar::log_level level;
};

static const std::map<seastar::log_level, std::string_view> log_level_names = {
        { seastar::log_level::trace, "trace" },
        { seastar::log_level::debug, "debug" },
        { seastar::log_level::info, "info" },
        { seastar::log_level::warn, "warn" },
        { seastar::log_level::error, "error" },
};

namespace fmt {
template <> struct formatter<wrapped_log_level> {
    using log_level = seastar::log_level;
    static constexpr size_t nr_levels = static_cast<size_t>(log_level::trace) + 1;
    static bool colored;

    // format specifier not supported
    template <typename ParseContext>
    constexpr auto parse(ParseContext &ctx) { return ctx.begin(); }

    template <typename FormatContext>
    auto format(wrapped_log_level wll, FormatContext& ctx) const {
        static seastar::internal::array_map<seastar::sstring, nr_levels> text = {
            { int(log_level::debug), "DEBUG" },
            { int(log_level::info),  "INFO " },
            { int(log_level::trace), "TRACE" },
            { int(log_level::warn),  "WARN " },
            { int(log_level::error), "ERROR" },
        };
        int index = static_cast<int>(wll.level);
        std::string_view name = text[index];
        static seastar::internal::array_map<text_style, nr_levels> style = {
            { int(log_level::debug), fg(terminal_color::green)  },
            { int(log_level::info),  fg(terminal_color::white)  },
            { int(log_level::trace), fg(terminal_color::blue)   },
            { int(log_level::warn),  fg(terminal_color::yellow) },
            { int(log_level::error), fg(terminal_color::red)    },
        };
        if (colored) {
            return fmt::format_to(ctx.out(), "{}",
                fmt::format(style[index], "{}", name));
        }
        return fmt::format_to(ctx.out(), "{}", name);
    }
};
bool formatter<wrapped_log_level>::colored = true;

auto formatter<seastar::log_level>::format(seastar::log_level level, format_context& ctx) const
    -> decltype(ctx.out()) {
    return fmt::format_to(ctx.out(), "{}", log_level_names.at(level));
}

}

namespace seastar {

namespace internal {

void log_buf::free_buffer() noexcept {
    if (_own_buf) {
        delete[] _begin;
    }
}

void log_buf::realloc_buffer_and_append(char c) noexcept {
  if (_alloc_failure) {
    // Already failed to reallocate once, don't try again
    return;
  }

  try {
    const auto old_size = size();
    const auto new_size = old_size * 2;

    auto new_buf = new char[new_size];
    std::memcpy(new_buf, _begin, old_size);
    free_buffer();

    _begin = new_buf;
    _current = _begin + old_size;
    _end = _begin + new_size;
    _own_buf = true;
    *_current++ = c;
  } catch (...) {
    _alloc_failure = true;
    std::string_view msg = "(log buffer allocation failure)";
    auto can_copy = std::min(msg.size(), size_t(_current - _begin));
    std::memcpy(_current - can_copy, msg.data(), can_copy);
  }
}

log_buf::log_buf()
    : _begin(new char[512])
    , _end(_begin + 512)
    , _current(_begin)
    , _own_buf(true)
{
}

log_buf::log_buf(char* external_buf, size_t size) noexcept
    : _begin(external_buf)
    , _end(_begin + size)
    , _current(_begin)
    , _own_buf(false)
{
}

log_buf::~log_buf() {
    free_buffer();
}

} // namespace internal

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

void validate(boost::any& v,
              const std::vector<std::string>& values,
              logger_ostream_type* target_type, int) {
    using namespace boost::program_options;
    validators::check_first_occurrence(v);
    auto s = validators::get_single_string(values);
    if (s == "none") {
        v = logger_ostream_type::none;
        return;
    } else if (s == "stdout") {
        v = logger_ostream_type::cout;
        return;
    } else if (s == "stderr") {
        v = logger_ostream_type::cerr;
        return;
    }
    throw validation_error(validation_error::invalid_option_value);
}

std::ostream& operator<<(std::ostream& os, logger_ostream_type lot) {
    switch (lot) {
    case logger_ostream_type::none: return os << "none";
    case logger_ostream_type::cout: return os << "stdout";
    case logger_ostream_type::cerr: return os << "stderr";
    default: abort();
    }
    return os;
}

static internal::log_buf::inserter_iterator print_no_timestamp(internal::log_buf::inserter_iterator it) {
    return it;
}

static internal::log_buf::inserter_iterator print_boot_timestamp(internal::log_buf::inserter_iterator it) {
    auto n = std::chrono::steady_clock::now().time_since_epoch() / 1us;
    return fmt::format_to(it, "{:10d}.{:06d}", n / 1000000, n % 1000000);
}

static internal::log_buf::inserter_iterator print_real_timestamp(internal::log_buf::inserter_iterator it) {
    struct a_second {
        time_t t;
        std::array<char, 32> static_buf; // big enough to hold '2023-01-14 15:06:33'
        internal::log_buf buf{static_buf.data(), static_buf.size()};
    };
    static thread_local a_second this_second;
    using clock = std::chrono::system_clock;
    auto n = clock::now();
    auto t = clock::to_time_t(n);
    if (this_second.t != t) {
        this_second.t = t;
        this_second.buf.clear();
        std::tm tm_local;
        if (!localtime_r(&t, &tm_local)) {
            throw fmt::format_error("time_t value out of range");
        }
        fmt::format_to(this_second.buf.back_insert_begin(), "{:%F %T}", tm_local);
    }
    auto ms = (n - clock::from_time_t(t)) / 1ms;
    return fmt::format_to(it, "{},{:03d}", this_second.buf.view(), ms);
}

static internal::log_buf::inserter_iterator (*print_timestamp)(internal::log_buf::inserter_iterator) = print_no_timestamp;


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

std::ostream* logger::_out = &std::cerr;
std::atomic<bool> logger::_ostream = { true };
std::atomic<bool> logger::_syslog = { false };
unsigned logger::_shard_field_width = 1;
#ifdef SEASTAR_BUILD_SHARED_LIBS
thread_local bool logger::silent = false;
#endif

logger::logger(sstring name) : _name(std::move(name)) {
    global_logger_registry().register_logger(this);
}

logger::logger(logger&& x) : _name(std::move(x._name)), _level(x._level.load(std::memory_order_relaxed)) {
    global_logger_registry().moved(&x, this);
}

logger::~logger() {
    global_logger_registry().unregister_logger(this);
}

static thread_local std::array<char, 8192> static_log_buf;

bool logger::rate_limit::rate_limited() {
    const auto now = clock::now();
    if (now < _next) {
        ++_dropped_messages;
        return true;
    }
    _next = now + _interval;
    return false;
}

logger::rate_limit::rate_limit(std::chrono::milliseconds interval)
    : _interval(interval), _next(clock::now())
{ }


namespace {

// A private connection to the system logger (/dev/log).
//
// libc's syslog() serializes all calls on a global lock, and shares a single
// socket among all threads; on a large machine that lock, and the shared
// socket's send buffer, become a contention point that can stall a reactor
// thread for a long time.  Instead, every shard keeps its own socket, so
// shards never wait for each other.
//
// The datagram we generate follows the traditional BSD syslog format
// (RFC 3164), which is what libc's syslog() emits and what every system
// logger understands:
//
//     <PRI>MMM dd hh:mm:ss tag[pid]: message
//
class syslog_socket {
#ifdef _PATH_LOG
    static constexpr const char* socket_path = _PATH_LOG;
#else
    static constexpr const char* socket_path = "/dev/log";
#endif
    // Retry interval after a failed attempt to connect to /dev/log, so that
    // a missing or unresponsive system logger doesn't cost us a socket() and
    // a connect() per log message.
    static constexpr auto reconnect_interval = std::chrono::seconds(1);
    using clock = std::chrono::steady_clock;

    int _fd = -1;
    clock::time_point _next_connect_attempt = clock::time_point::min();
    // Cached at connect() time; a fork() gives the child a fresh socket anyway.
    pid_t _pid = 0;
public:
    syslog_socket() = default;
    syslog_socket(const syslog_socket&) = delete;
    ~syslog_socket() {
        disconnect();
    }
    /// Sends a log message to the system logger.
    ///
    /// \param priority the syslog priority (facility | level) of the message
    /// \param msg the message body, without a trailing newline
    /// \return true if the message was handed over to the system logger, false
    ///         if the caller should fall back to libc's syslog()
    bool send(int priority, std::string_view msg) noexcept;
    /// Returns this shard's socket.
    static syslog_socket& local() noexcept {
        static thread_local syslog_socket sock;
        return sock;
    }
private:
    bool connect() noexcept;
    void disconnect() noexcept;
    // Formats the RFC 3164 header (priority, timestamp and tag) of a message.
    internal::log_buf::inserter_iterator print_header(internal::log_buf::inserter_iterator it, int priority) const;
};

void syslog_socket::disconnect() noexcept {
    if (_fd >= 0) {
        ::close(_fd);
        _fd = -1;
    }
}

bool syslog_socket::connect() noexcept {
    auto now = clock::now();
    if (now < _next_connect_attempt) {
        return false;
    }
    _next_connect_attempt = now + reconnect_interval;
    // SOCK_NONBLOCK: a full send buffer must never stall the reactor; we'd
    // rather drop the message (see the buffer-full handling in send()).
    int fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        return false;
    }
    sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    std::strcpy(addr.sun_path, socket_path);
    // A datagram socket has no handshake to perform, so this only records the
    // peer address and completes right away; SOCK_NONBLOCK notwithstanding,
    // there is no EINPROGRESS to wait for.
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return false;
    }
    _fd = fd;
    _pid = ::getpid();
    return true;
}

internal::log_buf::inserter_iterator
syslog_socket::print_header(internal::log_buf::inserter_iterator it, int priority) const {
    static const char* const month_names[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
    };
    auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm_local;
    // RFC 3164 wants a local time, space-padded day-of-month, and no year.
    if (localtime_r(&t, &tm_local)) {
        it = fmt::format_to(it, "<{}>{} {:2d} {:02d}:{:02d}:{:02d} ",
                priority, month_names[tm_local.tm_mon], tm_local.tm_mday,
                tm_local.tm_hour, tm_local.tm_min, tm_local.tm_sec);
    } else {
        // The timestamp is optional; the system logger will supply its own.
        it = fmt::format_to(it, "<{}>", priority);
    }
    return fmt::format_to(it, "{}[{}]:", program_invocation_short_name, _pid);
}

bool syslog_socket::send(int priority, std::string_view msg) noexcept {
    if (_fd < 0 && !connect()) {
        return false;
    }
    // Big enough for the priority, the timestamp and any sane program name.
    std::array<char, 256> header_buf;
    internal::log_buf header(header_buf.data(), header_buf.size());
    print_header(header.back_insert_begin(), priority);
    // The header and the message are sent as one datagram, without copying
    // the (potentially large) message.
    iovec iov[2] = {
        { const_cast<char*>(header.data()), header.size() },
        { const_cast<char*>(msg.data()), msg.size() },
    };
    msghdr mh = {};
    mh.msg_iov = iov;
    mh.msg_iovlen = 2;
    while (::sendmsg(_fd, &mh, MSG_NOSIGNAL) < 0) {
        switch (errno) {
        case EINTR:
            continue;
        case EAGAIN:
            // The system logger is not keeping up.  Dropping the message is
            // still better than stalling the reactor waiting for it.
            return true;
        default:
            // The system logger went away (restarted, most likely).  Try to
            // reconnect once, and fall back to syslog() if that fails too.
            disconnect();
            _next_connect_attempt = clock::time_point::min();
            if (!connect()) {
                return false;
            }
        }
    }
    return true;
}

} // anonymous namespace

void
logger::do_log(log_level level, log_writer& writer) {
    bool is_ostream_enabled = _ostream.load(std::memory_order_relaxed);
    bool is_syslog_enabled = _syslog.load(std::memory_order_relaxed);
    if(!is_ostream_enabled && !is_syslog_enabled) {
      return;
    }
    auto print_once = [&] (internal::log_buf::inserter_iterator it) {
      if (local_engine) {
          it = fmt::format_to(it, " [shard {:{}}:{}]", this_shard_id(), _shard_field_width, current_scheduling_group().short_name());
      }
      it = fmt::format_to(it, " {} - ", _name);
      return writer(it);
    };

    // Mainly this protects us from re-entrance via malloc()'s
    // oversized allocation warnings and failed allocation errors
    silencer be_silent;

    if (is_ostream_enabled) {
        internal::log_buf buf(static_log_buf.data(), static_log_buf.size());
        auto it = buf.back_insert_begin();
        it = fmt::format_to(it, "{} ", wrapped_log_level{level});
        it = print_timestamp(it);
        it = print_once(it);
        *it++ = '\n';
        *_out << buf.view();
        _out->flush();
    }
    if (is_syslog_enabled) {
        internal::log_buf buf(static_log_buf.data(), static_log_buf.size());
        auto it = buf.back_insert_begin();
        it = print_once(it);
        static internal::array_map<int, 20> level_map = {
                { int(log_level::debug), LOG_DEBUG },
                { int(log_level::info), LOG_INFO },
                { int(log_level::trace), LOG_DEBUG },  // no LOG_TRACE
                { int(log_level::warn), LOG_WARNING },
                { int(log_level::error), LOG_ERR },
        };
        int priority = LOG_USER | level_map[int(level)];
        // Reactor threads use a private socket, so that they neither contend
        // on syslog()'s global lock, nor stall on a send buffer filled by
        // another shard.  Other threads are few and are not latency sensitive,
        // so they can use syslog(); it is also the fallback for the case where
        // the system logger cannot be reached directly.
        if (!local_engine || !syslog_socket::local().send(priority, buf.view())) {
            // syslog() wants a null-terminated string, and interprets %
            // characters, so the message is passed as a parameter.
            *it = '\0';
            syslog(priority, "%s", buf.data());
        }
    }
}

void logger::failed_to_log(std::exception_ptr ex,
                           fmt::string_view fmt,
                           std::source_location loc) noexcept
{
    try {
        lambda_log_writer writer([ex = std::move(ex), fmt, loc] (internal::log_buf::inserter_iterator it) {
            it = fmt::format_to(it, "{}:{} @{}: failed to log message", loc.file_name(), loc.line(), loc.function_name());
            if (fmt.size() > 0) {
                it = fmt::format_to(it, ": fmt='{}'", fmt);
            }
            return fmt::format_to(it, ": {}", seastar::formattable(ex));
        });
        do_log(log_level::error, writer);
    } catch (...) {
        ++logging_failures;
    }
}

void
logger::set_ostream(std::ostream& out) noexcept {
    _out = &out;
}

void
logger::set_ostream_enabled(bool enabled) noexcept {
    _ostream.store(enabled, std::memory_order_relaxed);
}

void
logger::set_syslog_enabled(bool enabled) noexcept {
    _syslog.store(enabled, std::memory_order_relaxed);
}

void
logger::set_shard_field_width(unsigned width) noexcept {
    _shard_field_width = width;
}

void
logger::set_with_color(bool enabled) noexcept {
    fmt::formatter<wrapped_log_level>::colored = enabled;
}

bool logger::is_shard_zero() noexcept {
    return this_shard_id() == 0;
}

void
logger_registry::set_all_loggers_level(log_level level) {
    std::lock_guard<std::mutex> g(_mutex);
    for (auto&& l : _loggers | std::views::values) {
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
    auto ret = _loggers | std::views::keys;
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

    logger_ostream_type logger_ostream = s.stdout_enabled ? s.logger_ostream : logger_ostream_type::none;
    switch (logger_ostream) {
    case logger_ostream_type::none:
        logger::set_ostream_enabled(false);
        break;
    case logger_ostream_type::cout:
        logger::set_ostream(std::cout);
        logger::set_ostream_enabled(true);
        break;
    case logger_ostream_type::cerr:
        logger::set_ostream(std::cerr);
        logger::set_ostream_enabled(true);
        break;
    }
    logger::set_syslog_enabled(s.syslog_enabled);
    logger::set_with_color(s.with_color);

    switch (s.stdout_timestamp_style) {
    case logger_timestamp_style::none:
        print_timestamp = print_no_timestamp;
        break;
    case logger_timestamp_style::boot:
        print_timestamp = print_boot_timestamp;
        break;
    case logger_timestamp_style::real:
        print_timestamp = print_real_timestamp;
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
    return sstring(log_level_names.at(level));
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

void parse_map_associations(const std::string& v, std::function<void(std::string, std::string)> consume_key_value) {
    static const std::regex colon(":");

    std::sregex_token_iterator s(v.begin(), v.end(), colon, -1);
    const std::sregex_token_iterator e;
    while (s != e) {
        const sstring p = std::string(*s++);

        const auto i = p.find('=');
        if (i == sstring::npos) {
            throw bpo::invalid_option_value(p);
        }

        auto k = p.substr(0, i);
        auto v = p.substr(i + 1, p.size());
        consume_key_value(std::move(k), std::move(v));
    };
}

bpo::options_description get_options_description() {
    program_options::options_description_building_visitor descriptor;
    options(nullptr).describe(descriptor);
    return std::move(descriptor).get_options_description();
}

options::options(program_options::option_group* parent_group)
    : program_options::option_group(parent_group, "Logging options")
    , default_log_level(*this, "default-log-level",
             log_level::info,
             "Default log level for log messages. Valid values are trace, debug, info, warn, error."
             )
    , logger_log_level(*this, "logger-log-level",
             log_level_map{},
             "Map of logger name to log level. The format is \"NAME0=LEVEL0[:NAME1=LEVEL1:...]\". "
             "Valid logger names can be queried with --help-loggers. "
             "Valid values for levels are trace, debug, info, warn, error. "
             "This option can be specified multiple times."
            )
    , logger_stdout_timestamps(*this, "logger-stdout-timestamps", logger_timestamp_style::real,
                    "Select timestamp style for stdout logs: none|boot|real")
    , log_to_stdout(*this, "log-to-stdout", true, "Send log output to output stream, as selected by --logger-ostream-type")
    , logger_ostream_type(*this, "logger-ostream-type", logger_ostream_type::cerr,
            "Send log output to: none|stdout|stderr")
    , log_to_syslog(*this, "log-to-syslog", false, "Send log output to syslog.")
    , log_with_color(*this, "log-with-color", isatty(STDOUT_FILENO), "Print colored tag prefix in log message written to ostream")
{
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
    options opts(nullptr);
    program_options::variables_map_extracting_visitor visitor(vars);
    opts.mutate(visitor);
    return extract_settings(opts);
}

logging_settings extract_settings(const options& opts) {
    return logging_settings{
        opts.logger_log_level.get_value(),
        opts.default_log_level.get_value(),
        opts.log_to_stdout.get_value(),
        opts.log_to_syslog.get_value(),
        opts.log_with_color.get_value(),
        opts.logger_stdout_timestamps.get_value(),
        opts.logger_ostream_type.get_value(),
    };
}

}

}

auto fmt::formatter<seastar::internal::formattable_exception_ptr>::format(
        const seastar::internal::formattable_exception_ptr& fe, fmt::format_context& ctx) const
    -> decltype(ctx.out()) {
    auto out = ctx.out();
    const auto& eptr = fe.eptr;
    if (!eptr) {
        return fmt::format_to(out, "<no exception>");
    }
    try {
        std::rethrow_exception(eptr);
    } catch(...) {
        auto tp = abi::__cxa_current_exception_type();
        if (tp) {
            out = fmt::format_to(out, "{}", seastar::pretty_type_name(*tp));
        } else {
            // This case shouldn't happen...
            out = fmt::format_to(out, "<unknown exception>");
        }
        // Print more information on some familiar exception types
        try {
            throw;
        } catch (const seastar::nested_exception& ne) {
            out = fmt::format_to(out, ": {} (while cleaning up after {})",
                    seastar::formattable(ne.inner), seastar::formattable(ne.outer));
        } catch (const std::system_error& e) {
            out = fmt::format_to(out, " (error {}, {})", e.code(), e.what());
        } catch (const std::exception& e) {
            out = fmt::format_to(out, " ({})", e.what());
        } catch (...) {
            // no extra info
        }

        try {
            throw;
        } catch (const std::nested_exception& ne) {
            out = fmt::format_to(out, ": {}", seastar::formattable(ne.nested_ptr()));
        } catch (...) {
            // do nothing
        }
    }
    return out;
}

#ifdef SEASTAR_DEPRECATED_OSTREAM_FORMATTERS
namespace std {

std::ostream& operator<<(std::ostream& out, const std::exception_ptr& eptr) {
    return out << fmt::format("{}", seastar::formattable(eptr));
}

std::ostream& operator<<(std::ostream& out, const std::exception& e) {
    return out << fmt::format("{}", e);
}

std::ostream& operator<<(std::ostream& out, const std::system_error& e) {
    return out << fmt::format("{}", e);
}

}
#endif
