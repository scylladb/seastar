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
#pragma once

#include <seastar/core/sstring.hh>
#include <seastar/util/concepts.hh>
#include <seastar/util/log-impl.hh>
#include <seastar/core/lowres_clock.hh>
#include <seastar/util/std-compat.hh>

#include <unordered_map>
#include <exception>
#include <iosfwd>
#include <atomic>
#include <mutex>
#include <boost/lexical_cast.hpp>
#include <fmt/format.h>


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

// Boost doesn't auto-deduce the existence of the streaming operators for some reason

namespace boost {
template<>
seastar::log_level lexical_cast(const std::string& source);

}

namespace seastar {

class logger;
class logger_registry;

/// \brief Logger class for ostream or syslog.
///
/// Java style api for logging.
/// \code {.cpp}
/// static seastar::logger logger("lsa-api");
/// logger.info("Triggering compaction");
/// \endcode
/// The output format is: (depending on level)
/// DEBUG  %Y-%m-%d %T,%03d [shard 0] - "your msg" \n
///
/// It is possible to rate-limit log messages, see \ref logger::rate_limit.
class logger {
    sstring _name;
    std::atomic<log_level> _level = { log_level::info };
    static std::ostream* _out;
    static std::atomic<bool> _ostream;
    static std::atomic<bool> _syslog;

public:
    class log_writer {
    public:
        virtual ~log_writer() = default;
        virtual internal::log_buf::inserter_iterator operator()(internal::log_buf::inserter_iterator) = 0;
    };
    template <typename Func>
    SEASTAR_CONCEPT(requires requires (Func fn, internal::log_buf::inserter_iterator it) {
        it = fn(it);
    })
    class lambda_log_writer : public log_writer {
        Func _func;
    public:
        lambda_log_writer(Func&& func) : _func(std::forward<Func>(func)) { }
        virtual ~lambda_log_writer() override = default;
        virtual internal::log_buf::inserter_iterator operator()(internal::log_buf::inserter_iterator it) override { return _func(it); }
    };

    /// \cond internal
    /// \brief used to hold the log format string and the caller's source_location.
    struct format_info {
        /// implicitly construct format_info from a const char* format string.
        /// \param fmt - {fmt} style format string
        format_info(const char* format, compat::source_location loc = compat::source_location::current()) noexcept
            : format(format)
            , loc(loc)
        {}
        /// implicitly construct format_info from a std::string_view format string.
        /// \param fmt - {fmt} style format string_view
        format_info(std::string_view format, compat::source_location loc = compat::source_location::current()) noexcept
            : format(format)
            , loc(loc)
        {}
        /// implicitly construct format_info with no format string.
        format_info(compat::source_location loc = compat::source_location::current()) noexcept
            : format()
            , loc(loc)
        {}
        std::string_view format;
        compat::source_location loc;
    };

private:

    // We can't use an std::function<> as it potentially allocates.
    void do_log(log_level level, log_writer& writer);
    void failed_to_log(std::exception_ptr ex, format_info fmt) noexcept;
public:
    /// Apply a rate limit to log message(s)
    ///
    /// Pass this to \ref logger::log() to apply a rate limit to the message.
    /// The rate limit is applied to all \ref logger::log() calls this rate
    /// limit is passed to. Example:
    ///
    ///     void handle_request() {
    ///         static thread_local logger::rate_limit my_rl(std::chrono::seconds(10));
    ///         // ...
    ///         my_log.log(log_level::info, my_rl, "a message we don't want to log on every request, only at most once each 10 seconds");
    ///         // ...
    ///     }
    ///
    /// The rate limit ensures that at most one message per interval will be
    /// logged. If there were messages dropped due to rate-limiting the
    /// following snippet will be prepended to the first non-dropped log
    /// messages:
    ///
    ///     (rate limiting dropped $N similar messages)
    ///
    /// Where $N is the number of messages dropped.
    class rate_limit {
        friend class logger;

        using clock = lowres_clock;

    private:
        clock::duration _interval;
        clock::time_point _next;
        uint64_t _dropped_messages = 0;

    private:
        bool check();
        bool has_dropped_messages() const { return bool(_dropped_messages); }
        uint64_t get_and_reset_dropped_messages() {
            return std::exchange(_dropped_messages, 0);
        }

    public:
        explicit rate_limit(std::chrono::milliseconds interval);
    };

public:
    explicit logger(sstring name);
    logger(logger&& x);
    ~logger();

    bool is_shard_zero() noexcept;

    /// Test if desired log level is enabled
    ///
    /// \param level - enum level value (info|error...)
    /// \return true if the log level has been enabled.
    bool is_enabled(log_level level) const noexcept {
        return __builtin_expect(level <= _level.load(std::memory_order_relaxed), false);
    }

    /// logs to desired level if enabled, otherwise we ignore the log line
    ///
    /// \param fmt - {fmt} style format string (implictly converted to struct logger::format_info)
    ///              or a logger::format_info passed down the call chain.
    /// \param args - args to print string
    ///
    template <typename... Args>
    void log(log_level level, format_info fmt, Args&&... args) noexcept {
        if (is_enabled(level)) {
            try {
                lambda_log_writer writer([&] (internal::log_buf::inserter_iterator it) {
                    return fmt::format_to(it, fmt.format, std::forward<Args>(args)...);
                });
                do_log(level, writer);
            } catch (...) {
                failed_to_log(std::current_exception(), std::move(fmt));
            }
        }
    }

    /// logs with a rate limit to desired level if enabled, otherwise we ignore the log line
    ///
    /// If there were messages dropped due to rate-limiting the following snippet
    /// will be prepended to the first non-dropped log messages:
    ///
    ///     (rate limiting dropped $N similar messages)
    ///
    /// Where $N is the number of messages dropped.
    ///
    /// \param rl - the \ref rate_limit to apply to this log
    /// \param fmt - {fmt} style format string (implictly converted to struct logger::format_info)
    ///              or a logger::format_info passed down the call chain.
    /// \param args - args to print string
    ///
    template <typename... Args>
    void log(log_level level, rate_limit& rl, format_info fmt, Args&&... args) noexcept {
        if (is_enabled(level) && rl.check()) {
            try {
                lambda_log_writer writer([&] (internal::log_buf::inserter_iterator it) {
                    if (rl.has_dropped_messages()) {
                        it = fmt::format_to(it, "(rate limiting dropped {} similar messages) ", rl.get_and_reset_dropped_messages());
                    }
                    return fmt::format_to(it, fmt.format, std::forward<Args>(args)...);
                });
                do_log(level, writer);
            } catch (...) {
                failed_to_log(std::current_exception(), std::move(fmt));
            }
        }
    }

    /// \cond internal
    /// logs to desired level if enabled, otherwise we ignore the log line
    ///
    /// \param writer a function which writes directly to the underlying log buffer
    /// \param fmt - optional logger::format_info passed down the call chain.
    ///
    /// This is a low level method for use cases where it is very important to
    /// avoid any allocations. The \arg writer will be passed a
    /// internal::log_buf::inserter_iterator that allows it to write into the log
    /// buffer directly, avoiding the use of any intermediary buffers.
    void log(log_level level, log_writer& writer, format_info fmt = {}) noexcept {
        if (is_enabled(level)) {
            try {
                do_log(level, writer);
            } catch (...) {
                failed_to_log(std::current_exception(), std::move(fmt));
            }
        }
    }
    /// logs to desired level if enabled, otherwise we ignore the log line
    ///
    /// \param writer a function which writes directly to the underlying log buffer
    /// \param fmt - optional logger::format_info passed down the call chain.
    ///
    /// This is a low level method for use cases where it is very important to
    /// avoid any allocations. The \arg writer will be passed a
    /// internal::log_buf::inserter_iterator that allows it to write into the log
    /// buffer directly, avoiding the use of any intermediary buffers.
    /// This is rate-limited version, see \ref rate_limit.
    void log(log_level level, rate_limit& rl, log_writer& writer, format_info fmt = {}) noexcept {
        if (is_enabled(level) && rl.check()) {
            try {
                lambda_log_writer writer_wrapper([&] (internal::log_buf::inserter_iterator it) {
                    if (rl.has_dropped_messages()) {
                        it = fmt::format_to(it, "(rate limiting dropped {} similar messages) ", rl.get_and_reset_dropped_messages());
                    }
                    return writer(it);
                });
                do_log(level, writer_wrapper);
            } catch (...) {
                failed_to_log(std::current_exception(), std::move(fmt));
            }
        }
    }
    /// \endcond

    /// Log with error tag:
    /// ERROR  %Y-%m-%d %T,%03d [shard 0] - "your msg" \n
    ///
    /// \param fmt - {fmt} style format string (implictly converted to struct logger::format_info)
    ///              or a logger::format_info passed down the call chain.
    /// \param args - args to print string
    ///
    template <typename... Args>
    void error(format_info fmt, Args&&... args) noexcept {
        log(log_level::error, std::move(fmt), std::forward<Args>(args)...);
    }
    /// Log with warning tag:
    /// WARN  %Y-%m-%d %T,%03d [shard 0] - "your msg" \n
    ///
    /// \param fmt - {fmt} style format string (implictly converted to struct logger::format_info)
    ///              or a logger::format_info passed down the call chain.
    /// \param args - args to print string
    ///
    template <typename... Args>
    void warn(format_info fmt, Args&&... args) noexcept {
        log(log_level::warn, std::move(fmt), std::forward<Args>(args)...);
    }
    /// Log with info tag:
    /// INFO  %Y-%m-%d %T,%03d [shard 0] - "your msg" \n
    ///
    /// \param fmt - {fmt} style format string (implictly converted to struct logger::format_info)
    ///              or a logger::format_info passed down the call chain.
    /// \param args - args to print string
    ///
    template <typename... Args>
    void info(format_info fmt, Args&&... args) noexcept {
        log(log_level::info, std::move(fmt), std::forward<Args>(args)...);
    }
    /// Log with info tag on shard zero only:
    /// INFO  %Y-%m-%d %T,%03d [shard 0] - "your msg" \n
    ///
    /// \param fmt - {fmt} style format string (implictly converted to struct logger::format_info)
    ///              or a logger::format_info passed down the call chain.
    /// \param args - args to print string
    ///
    template <typename... Args>
    void info0(format_info fmt, Args&&... args) noexcept {
        if (is_shard_zero()) {
            log(log_level::info, std::move(fmt), std::forward<Args>(args)...);
        }
    }
    /// Log with debug tag:
    /// DEBUG  %Y-%m-%d %T,%03d [shard 0] - "your msg" \n
    ///
    /// \param fmt - {fmt} style format string (implictly converted to struct logger::format_info)
    ///              or a logger::format_info passed down the call chain.
    /// \param args - args to print string
    ///
    template <typename... Args>
    void debug(format_info fmt, Args&&... args) noexcept {
        log(log_level::debug, std::move(fmt), std::forward<Args>(args)...);
    }
    /// Log with trace tag:
    /// TRACE  %Y-%m-%d %T,%03d [shard 0] - "your msg" \n
    ///
    /// \param fmt - {fmt} style format string (implictly converted to struct logger::format_info)
    ///              or a logger::format_info passed down the call chain.
    /// \param args - args to print string
    ///
    template <typename... Args>
    void trace(format_info fmt, Args&&... args) noexcept {
        log(log_level::trace, std::move(fmt), std::forward<Args>(args)...);
    }

    /// \return name of the logger. Usually one logger per module
    ///
    const sstring& name() const noexcept {
        return _name;
    }

    /// \return current log level for this logger
    ///
    log_level level() const noexcept {
        return _level.load(std::memory_order_relaxed);
    }

    /// \param level - set the log level
    ///
    void set_level(log_level level) noexcept {
        _level.store(level, std::memory_order_relaxed);
    }

    /// Set output stream, default is std::cerr
    static void set_ostream(std::ostream& out) noexcept;

    /// Also output to ostream. default is true
    static void set_ostream_enabled(bool enabled) noexcept;

    /// Also output to stdout. default is true
    [[deprecated("Use set_ostream_enabled instead")]]
    static void set_stdout_enabled(bool enabled) noexcept;

    /// Also output to syslog. default is false
    ///
    /// NOTE: syslog() can block, which will stall the reactor thread.
    ///       this should be rare (will have to fill the pipe buffer
    ///       before syslogd can clear it) but can happen.
    static void set_syslog_enabled(bool enabled) noexcept;
};

/// \brief used to keep a static registry of loggers
/// since the typical use case is to do:
/// \code {.cpp}
/// static seastar::logger("my_module");
/// \endcode
/// this class is used to wrap around the static map
/// that holds pointers to all logs
///
class logger_registry {
    mutable std::mutex _mutex;
    std::unordered_map<sstring, logger*> _loggers;
public:
    /// loops through all registered loggers and sets the log level
    /// Note: this method locks
    ///
    /// \param level - desired level: error,info,...
    void set_all_loggers_level(log_level level);

    /// Given a name for a logger returns the log_level enum
    /// Note: this method locks
    ///
    /// \return log_level for the given logger name
    log_level get_logger_level(sstring name) const;

    /// Sets the log level for a given logger
    /// Note: this method locks
    ///
    /// \param name - name of logger
    /// \param level - desired level of logging
    void set_logger_level(sstring name, log_level level);

    /// Returns a list of registered loggers
    /// Note: this method locks
    ///
    /// \return all registered loggers
    std::vector<sstring> get_all_logger_names();

    /// Registers a logger with the static map
    /// Note: this method locks
    ///
    void register_logger(logger* l);
    /// Unregisters a logger with the static map
    /// Note: this method locks
    ///
    void unregister_logger(logger* l);
    /// Swaps the logger given the from->name() in the static map
    /// Note: this method locks
    ///
    void moved(logger* from, logger* to);
};

logger_registry& global_logger_registry();

enum class logger_timestamp_style {
    none,
    boot,
    real,
};

enum class logger_ostream_type {
    none,
    stdout,
    stderr,
};

struct logging_settings final {
    std::unordered_map<sstring, log_level> logger_levels;
    log_level default_level;
    bool stdout_enabled;
    bool syslog_enabled;
    logger_timestamp_style stdout_timestamp_style = logger_timestamp_style::real;
    logger_ostream_type logger_ostream = logger_ostream_type::stderr;
};

/// Shortcut for configuring the logging system all at once.
///
void apply_logging_settings(const logging_settings&);

/// \cond internal

extern thread_local uint64_t logging_failures;

sstring pretty_type_name(const std::type_info&);

sstring level_name(log_level level);

template <typename T>
class logger_for : public logger {
public:
    logger_for() : logger(pretty_type_name(typeid(T))) {}
};

/// \endcond
} // end seastar namespace

// Pretty-printer for exceptions to be logged, e.g., std::current_exception().
namespace std {
std::ostream& operator<<(std::ostream&, const std::exception_ptr&);
std::ostream& operator<<(std::ostream&, const std::exception&);
std::ostream& operator<<(std::ostream&, const std::system_error&);
}

/// @}
