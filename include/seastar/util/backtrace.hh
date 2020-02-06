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

#include <execinfo.h>
#include <iosfwd>
#include <boost/container/static_vector.hpp>

#include <seastar/core/sstring.hh>
#include <seastar/core/print.hh>
#include <seastar/core/scheduling.hh>

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

bool operator==(const frame& a, const frame& b);


// If addr doesn't seem to belong to any of the provided shared objects, it
// will be considered as part of the executable.
frame decorate(uintptr_t addr);

// Invokes func for each frame passing it as argument.
template<typename Func>
void backtrace(Func&& func) noexcept(noexcept(func(frame()))) {
    constexpr size_t max_backtrace = 100;
    void* buffer[max_backtrace];
    int n = ::backtrace(buffer, max_backtrace);
    for (int i = 0; i < n; ++i) {
        auto ip = reinterpret_cast<uintptr_t>(buffer[i]);
        func(decorate(ip - 1));
    }
}

class saved_backtrace {
public:
    using vector_type = boost::container::static_vector<frame, 64>;
private:
    vector_type _frames;
    scheduling_group _sg;
public:
    saved_backtrace() = default;
    saved_backtrace(vector_type f, scheduling_group sg) : _frames(std::move(f)), _sg(sg) {}
    size_t hash() const;

    friend std::ostream& operator<<(std::ostream& out, const saved_backtrace&);

    bool operator==(const saved_backtrace& o) const {
        return _frames == o._frames;
    }

    bool operator!=(const saved_backtrace& o) const {
        return !(*this == o);
    }
};

}

namespace std {

template<>
struct hash<seastar::saved_backtrace> {
    size_t operator()(const seastar::saved_backtrace& b) const {
        return b.hash();
    }
};

}

namespace seastar {

saved_backtrace current_backtrace() noexcept;
std::ostream& operator<<(std::ostream& out, const saved_backtrace& b);

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
        assert(_backtrace);
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
template <class Exc, typename... Args>
std::exception_ptr make_backtraced_exception_ptr(Args&&... args) {
    using exc_type = std::decay_t<Exc>;
    static_assert(std::is_base_of<std::exception, exc_type>::value,
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
template <class Exc, typename... Args>
[[noreturn]]
void
throw_with_backtrace(Args&&... args) {
    std::rethrow_exception(make_backtraced_exception_ptr<Exc>(std::forward<Args>(args)...));
};

}
