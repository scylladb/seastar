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
 * under the License.// Custom "validator" that gets called by the internals of Boost.Test. This allows for reading associations into an
// unordered map and for multiple occurances of the option name to appear and be merged.
 */
/*
 * Copyright (C) 2017 ScyllaDB
 */

#include <seastar/util/program-options.hh>
#include <seastar/util/log-cli.hh>

#include <regex>

namespace bpo = boost::program_options;

namespace seastar {

namespace program_options {

sstring get_or_default(const string_map& ss, const sstring& key, const sstring& def) {
    const auto iter = ss.find(key);
    if (iter != ss.end()) {
        return iter->second;
    }

    return def;
}

void validate(boost::any& out, const std::vector<std::string>& in, string_map*, int) {
    if (out.empty()) {
        out = boost::any(string_map());
    }

    auto* ss = boost::any_cast<string_map>(&out);

    for (const auto& s : in) {
        log_cli::parse_map_associations(s, [&ss] (std::string k, std::string v) { (*ss)[std::move(k)] = std::move(v); });
    }
}

std::ostream& operator<<(std::ostream& os, const string_map& ss) {
    int n = 0;

    for (const auto& e : ss) {
        if (n > 0) {
            os << ":";
        }

        os << e.first << "=" << e.second;
        ++n;
    }

    return os;
}

std::istream& operator>>(std::istream& is, string_map& ss) {
    std::string str;
    is >> str;

    log_cli::parse_map_associations(str, [&ss] (std::string k, std::string v) { ss[std::move(k)] = std::move(v); });
    return is;
}

option_group::option_group(option_group* parent, std::string name)
    : _parent(parent), _used(true), _name(std::move(name)) {
    if (_parent) {
        _parent->_subgroups.push_back(*this);
    }
}

option_group::option_group(option_group* parent, std::string name, unused)
    : _parent(parent), _used(false), _name(std::move(name)) {
    if (_parent) {
        _parent->_subgroups.push_back(*this);
    }
}

option_group::option_group(option_group&& o)
    : _parent(o._parent), _used(o._used), _name(std::move(o._name))
{
    for (auto& val : o._values) {
        val._group = this;
    }
    for (auto& grp : o._subgroups) {
        grp._parent = this;
    }
    unlink();
    if (_parent) {
        _parent->_subgroups.push_back(*this);
    }
}

void option_group::describe(options_descriptor& descriptor) const {
    if (descriptor.visit_group_start(_name, _used)) {
        for (auto& value : _values) {
            value.describe(descriptor);
        }
        for (auto& grp : _subgroups) {
            grp.describe(descriptor);
        }
    }
    descriptor.visit_group_end();
}

void option_group::mutate(options_mutator& mutator) {
    if (mutator.visit_group_start(_name, _used)) {
        for (auto& value : _values) {
            value.mutate(mutator);
        }
        for (auto& grp : _subgroups) {
            grp.mutate(mutator);
        }
    }
    mutator.visit_group_end();
}

basic_value::basic_value(option_group& group, bool used, std::string name, std::string description)
    : _group(&group), _used(used), _name(std::move(name)), _description(std::move(description))
{
    _group->_values.push_back(*this);
}

basic_value::basic_value(basic_value&& o)
    : _group(o._group), _used(o._used), _name(std::move(o._name)), _description(std::move(o._description))
{
    unlink();
    _group->_values.push_back(*this);
}

void basic_value::describe(options_descriptor& descriptor) const {
    if (descriptor.visit_value_metadata(_name, _description, _used)) {
        do_describe(descriptor);
    }
}

void basic_value::mutate(options_mutator& mutator) {
    if (mutator.visit_value_metadata(_name, _used)) {
        do_mutate(mutator);
    }
}

}

}
