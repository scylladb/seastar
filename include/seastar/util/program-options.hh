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

#include <seastar/core/sstring.hh>

#include <boost/any.hpp>
#include <boost/program_options.hpp>

#include <string>
#include <unordered_map>
#include <vector>

namespace seastar {

namespace program_options {

///
/// \brief Wrapper for command-line options with arbitrary string associations.
///
/// This type, to be used with Boost.Program_options, will result in an option that stores an arbitrary number of
/// string associations.
///
/// Values are specified in the form "key0=value0:[key1=value1:...]". Options of this type can be specified multiple
/// times, and the values will be merged (with the last-provided value for a key taking precedence).
///
/// \note We need a distinct type (rather than a simple type alias) for overload resolution in the implementation, but
/// advertizing our inheritance of \c std::unordered_map would introduce the possibility of memory leaks since STL
/// containers do not declare virtual destructors.
///
class string_map final : private std::unordered_map<sstring, sstring> {
private:
    using base = std::unordered_map<sstring, sstring>;
public:
    using base::value_type;
    using base::key_type;
    using base::mapped_type;

    using base::base;
    using base::at;
    using base::find;
    using base::count;
    using base::emplace;
    using base::clear;
    using base::operator[];
    using base::begin;
    using base::end;

    friend bool operator==(const string_map&, const string_map&);
    friend bool operator!=(const string_map&, const string_map&);
};

inline bool operator==(const string_map& lhs, const string_map& rhs) {
    return static_cast<const string_map::base&>(lhs) == static_cast<const string_map::base&>(rhs);
}

inline bool operator!=(const string_map& lhs, const string_map& rhs) {
    return !(lhs == rhs);
}

///
/// \brief Query the value of a key in a \c string_map, or a default value if the key doesn't exist.
///
sstring get_or_default(const string_map&, const sstring& key, const sstring& def = sstring());

std::istream& operator>>(std::istream& is, string_map&);
std::ostream& operator<<(std::ostream& os, const string_map&);

/// \cond internal

//
// Required implementation hook for Boost.Program_options.
//
void validate(boost::any& out, const std::vector<std::string>& in, string_map*, int);

/// \endcond

}

}
