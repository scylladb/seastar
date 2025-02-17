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
 * Copyright 2016 ScyllaDB
 */

#pragma once

#include <seastar/core/format.hh>
#include <seastar/core/sstring.hh>
#include <seastar/core/scheduling.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/util/assert.hh>
#include <seastar/util/modules.hh>

#ifndef SEASTAR_MODULE
#if __has_include(<execinfo.h>)
#include <execinfo.h>
#define HAVE_EXECINFO
#endif
#include <iosfwd>
#include <memory>
#include <variant>
#include <boost/container/static_vector.hpp>
#include <fmt/ostream.h>
#endif

namespace seastar {

struct shared_object {
    sstring name;
    uintptr_t begin;
    uintptr_t end; // C++-style, last addr + 1
};

struct frame {
    const shared_object* so;
    uintptr_t addr;
};

bool operator==(const frame& a, const frame& b) noexcept;


// If addr doesn't seem to belong to any of the provided shared objects, it
// will be considered as part of the executable.
frame decorate(uintptr_t addr) noexcept;

// Invokes func for each frame passing it as argument.
SEASTAR_MODULE_EXPORT
template<typename Func>
void backtrace(Func&& func) noexcept(noexcept(func(frame()))) {
#ifdef HAVE_EXECINFO
    constexpr size_t max_backtrace = 100;
    void* buffer[max_backtrace];
    int n = ::backtrace(buffer, max_backtrace);
    for (int i = 0; i < n; ++i) {
        auto ip = reinterpret_cast<uintptr_t>(buffer[i]);
        func(decorate(ip - 1));
    }
#else
// Not implemented yet
#define SEASTAR_BACKTRACE_UNIMPLEMENTED
#endif
}

// Represents a call stack of a single thread.
SEASTAR_MODULE_EXPORT
class simple_backtrace {
public:
    using vector_type = boost::container::static_vector<frame, 64>;
private:
    vector_type _frames;
    size_t _hash = 0;
    char _delimeter = ' ';
private:
    size_t calculate_hash() const noexcept;
public:
    simple_backtrace(vector_type f) noexcept : _frames(std::move(f)), _hash(calculate_hash()) {}
    simple_backtrace() noexcept = default;
    [[deprecated]] simple_backtrace(vector_type f, char delimeter) : _frames(std::move(f)), _hash(calculate_hash()), _delimeter(delimeter) {}
    [[deprecated]] simple_backtrace(char delimeter) : _delimeter(delimeter) {}

    size_t hash() const noexcept { return _hash; }
    char delimeter() const noexcept { return _delimeter; }

    friend fmt::formatter<simple_backtrace>;

    bool operator==(const simple_backtrace& o) const noexcept {
        return _hash == o._hash && _frames == o._frames;
    }

    bool operator!=(const simple_backtrace& o) const noexcept {
        return !(*this == o);
    }
};

using shared_backtrace = seastar::lw_shared_ptr<simple_backtrace>;

// Represents a task object inside a tasktrace.
class task_entry {
    const std::type_info* _task_type;
public:
    task_entry(const std::type_info& ti) noexcept
        : _task_type(&ti)
    { }

    friend fmt::formatter<task_entry>;

    bool operator==(const task_entry& o) const noexcept {
        return *_task_type == *o._task_type;
    }

    bool operator!=(const task_entry& o) const noexcept {
        return !(*this == o);
    }

    size_t hash() const noexcept { return _task_type->hash_code(); }
};

// Extended backtrace which consists of a backtrace of the currently running task
// and information about the chain of tasks waiting for the current operation to complete.
SEASTAR_MODULE_EXPORT
class tasktrace {
public:
    using entry = std::variant<shared_backtrace, task_entry>;
    using vector_type = boost::container::static_vector<entry, 16>;
private:
    simple_backtrace _main;
    vector_type _prev;
    scheduling_group _sg;
    size_t _hash;
public:
    tasktrace() = default;
    tasktrace(simple_backtrace main, vector_type prev, size_t prev_hash, scheduling_group sg) noexcept;
    tasktrace(const tasktrace&) = default;
    tasktrace& operator=(const tasktrace&) = default;
    ~tasktrace();

    size_t hash() const noexcept { return _hash; }
    char delimeter() const noexcept { return _main.delimeter(); }

    friend fmt::formatter<tasktrace>;

    bool operator==(const tasktrace& o) const noexcept;

    bool operator!=(const tasktrace& o) const noexcept {
        return !(*this == o);
    }
};

}

namespace std {

SEASTAR_MODULE_EXPORT
template<>
struct hash<seastar::simple_backtrace> {
    size_t operator()(const seastar::simple_backtrace& b) const {
        return b.hash();
    }
};

SEASTAR_MODULE_EXPORT
template<>
struct hash<seastar::tasktrace> {
    size_t operator()(const seastar::tasktrace& b) const {
        return b.hash();
    }
};

}

template <> struct fmt::formatter<seastar::frame> {
    constexpr auto parse(format_parse_context& ctx) { return ctx.begin(); }
    auto format(const seastar::frame&, fmt::format_context& ctx) const -> decltype(ctx.out());
};
template <> struct fmt::formatter<seastar::simple_backtrace> {
    constexpr auto parse(format_parse_context& ctx) { return ctx.begin(); }
    auto format(const seastar::simple_backtrace&, fmt::format_context& ctx) const -> decltype(ctx.out());
};
template <> struct fmt::formatter<seastar::tasktrace> {
    constexpr auto parse(format_parse_context& ctx) { return ctx.begin(); }
    auto format(const seastar::tasktrace&, fmt::format_context& ctx) const -> decltype(ctx.out());
};
template <> struct fmt::formatter<seastar::task_entry> {
    constexpr auto parse(format_parse_context& ctx) { return ctx.begin(); }
    auto format(const seastar::task_entry&, fmt::format_context& ctx) const -> decltype(ctx.out());
};

namespace seastar {

using saved_backtrace = tasktrace;

saved_backtrace current_backtrace() noexcept;

tasktrace current_tasktrace() noexcept;

// Collects backtrace only within the currently executing task.
simple_backtrace current_backtrace_tasklocal() noexcept;

std::ostream& operator<<(std::ostream& out, const tasktrace& b);

namespace internal {

template<class Exc>
class backtraced : public Exc {
    std::shared_ptr<sstring> _backtrace;
public:
    template<typename... Args>
    backtraced(Args&&... args)
            : Exc(std::forward<Args>(args)...)
            , _backtrace(std::make_shared<sstring>(format("{} Backtrace: {}", Exc::what(), current_backtrace()))) {}

    /**
     * Returns the original exception message with a backtrace appended to it
     *
     * @return original exception message followed by a backtrace
     */
    virtual const char* what() const noexcept override {
        SEASTAR_ASSERT(_backtrace);
        return _backtrace->c_str();
    }
};

}


/// Create an exception pointer of unspecified type that is derived from Exc type
/// with a backtrace attached to its message.
///
/// \tparam Exc exception type to be caught at the receiving side
/// \tparam Args types of arguments forwarded to the constructor of Exc
/// \param args arguments forwarded to the constructor of Exc
/// \return std::exception_ptr containing the exception with the backtrace.
SEASTAR_MODULE_EXPORT
template <class Exc, typename... Args>
std::exception_ptr make_backtraced_exception_ptr(Args&&... args) {
    using exc_type = std::decay_t<Exc>;
    static_assert(std::is_base_of_v<std::exception, exc_type>,
            "throw_with_backtrace only works with exception types");
    return std::make_exception_ptr<internal::backtraced<exc_type>>(Exc(std::forward<Args>(args)...));
}

    /**
     * Throws an exception of unspecified type that is derived from the Exc type
     * with a backtrace attached to its message
     *
     * @tparam Exc exception type to be caught at the receiving side
     * @tparam Args types of arguments forwarded to the constructor of Exc
     * @param args arguments forwarded to the constructor of Exc
     * @return never returns (throws an exception)
     */
SEASTAR_MODULE_EXPORT
template <class Exc, typename... Args>
[[noreturn]]
void
throw_with_backtrace(Args&&... args) {
    std::rethrow_exception(make_backtraced_exception_ptr<Exc>(std::forward<Args>(args)...));
};

}
