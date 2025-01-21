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
 * Copyright (C) 2021 Cloudius Systems, Ltd.
 */

#ifdef SEASTAR_MODULE
module;
#include <cstdlib>
#include <string>
#include <stdexcept>
#include <boost/program_options.hpp>
#include <boost/type.hpp>
#include <fmt/format.h>
module seastar;
#else
#include <boost/type.hpp>

#include "core/program_options.hh"

#include <seastar/util/log-cli.hh>
#include <seastar/util/memory_diagnostics.hh>
#include <seastar/core/reactor_config.hh>
#include <seastar/core/resource.hh>
#endif

namespace seastar::program_options {

namespace {

const char* to_string(memory::alloc_failure_kind val) {
    switch (val) {
        case memory::alloc_failure_kind::none: return "none";
        case memory::alloc_failure_kind::critical: return "critical";
        case memory::alloc_failure_kind::all: return "all";
    }
    std::abort();
}

const char* to_string(log_level val) {
    switch (val) {
        case log_level::error: return "error";
        case log_level::warn: return "warn";
        case log_level::info: return "info";
        case log_level::debug: return "debug";
        case log_level::trace: return "trace";
    }
    std::abort();
}

const char* to_string(logger_timestamp_style val) {
    switch (val) {
        case logger_timestamp_style::none: return "none";
        case logger_timestamp_style::boot: return "boot";
        case logger_timestamp_style::real: return "real";
    }
    std::abort();
}

const char* to_string(logger_ostream_type val) {
    switch (val) {
        case logger_ostream_type::none: return "none";
        case logger_ostream_type::cout: return "stdout";
        case logger_ostream_type::cerr: return "stderr";
    }
    std::abort();
}

memory::alloc_failure_kind from_string(const std::string& val, boost::type<memory::alloc_failure_kind>) {
    if (val == "none") {
        return memory::alloc_failure_kind::none;
    } else if (val == "critical") {
        return memory::alloc_failure_kind::critical;
    } else if (val == "all") {
        return memory::alloc_failure_kind::all;
    }
    throw std::runtime_error(fmt::format("Invalid value for enum memory::alloc_failure_kind: {}", val));
}

log_level from_string(const std::string& val, boost::type<log_level>) {
    if (val == "error") {
        return log_level::error;
    } else if (val == "warn") {
        return log_level::warn;
    } else if (val == "info") {
        return log_level::info;
    } else if (val == "debug") {
        return log_level::debug;
    } else if (val == "trace") {
        return log_level::trace;
    }
    throw std::runtime_error(fmt::format("Invalid value for enum log_level: {}", val));
}

logger_timestamp_style from_string(const std::string& val, boost::type<logger_timestamp_style>) {
    if (val == "none") {
        return logger_timestamp_style::none;
    } else if (val == "boot") {
        return logger_timestamp_style::boot;
    } else if (val == "real") {
        return logger_timestamp_style::real;
    }
    throw std::runtime_error(fmt::format("Invalid value for enum logger_timestamp_style: {}", val));
}

logger_ostream_type from_string(const std::string& val, boost::type<logger_ostream_type>) {
    if (val == "none") {
        return logger_ostream_type::none;
    } else if (val == "stdout") {
        return logger_ostream_type::cout;
    } else if (val == "stderr") {
        return logger_ostream_type::cerr;
    }
    throw std::runtime_error(fmt::format("Invalid value for enum logger_ostream_type: {}", val));
}

template <typename Type>
void describe_value(bpo::options_description& opts, const std::string& name, const std::string& description, const Type& default_value) {
    opts.add_options()(name.c_str(), boost::program_options::value<Type>()->default_value(default_value), description.c_str());
}

template <typename Type>
void describe_value(bpo::options_description& opts, const options_description_building_visitor::value_metadata& d, const Type& default_value) {
    describe_value(opts, d.name, d.description, default_value);
}

template <typename Type>
void describe_value(bpo::options_description& opts, const std::string& name, const std::string& description) {
    opts.add_options()(name.c_str(), boost::program_options::value<Type>(), description.c_str());
}

template <typename Type>
void describe_value(bpo::options_description& opts, const options_description_building_visitor::value_metadata& d) {
    describe_value<Type>(opts, d.name, d.description);
}

template <typename Type>
void describe_value_maybe_default(bpo::options_description& opts, const std::string& name, const std::string& description, const Type* default_value) {
    if (default_value) {
        describe_value(opts, name, description, *default_value);
    } else {
        describe_value<Type>(opts, name, description);
    }
}

template <typename Type>
void describe_value_maybe_default(bpo::options_description& opts, const options_description_building_visitor::value_metadata& d, const Type* default_value) {
    describe_value_maybe_default(opts, d.name, d.description, default_value);
}

template <typename Enum>
void describe_enum_value(bpo::options_description& opts, const options_description_building_visitor::value_metadata& d, const Enum* default_value) {
    if (default_value) {
        opts.add_options()(d.name.c_str(), boost::program_options::value<std::string>()->default_value(to_string(*default_value)), d.description.c_str());
    } else {
        opts.add_options()(d.name.c_str(), boost::program_options::value<std::string>(), d.description.c_str());
    }
}

template <typename T>
bool extract_value(const bpo::variables_map& values, const std::string& current_name, T& val) {
    auto it = values.find(current_name);
    if (it == values.end() || it->second.defaulted()) {
        return false;
    }
    val = it->second.as<T>();
    return true;
}

template <typename T>
bool extract_enum_value(const bpo::variables_map& values, const std::string& current_name, T& val) {
    auto it = values.find(current_name);
    if (it == values.end() || it->second.defaulted()) {
        return false;
    }
    val = from_string(it->second.as<std::string>(), boost::type<T>{});
    return true;
}

} // anonymous namespace

bool options_description_building_visitor::visit_group_start(const std::string& name, bool used) {
    _groups.push({name, bpo::options_description(name.c_str()), used});
    return used;
}
void options_description_building_visitor::visit_group_end() {
    if (_groups.size() == 1) {
        return;
    }
    auto grp = std::move(_groups.top());
    _groups.pop();
    if (grp.used && grp.values) {
        _groups.top().description.add(std::move(grp.description));
    }
}

bool options_description_building_visitor::visit_value_metadata(const std::string& name, const std::string& description, bool used) {
    if (!used) {
        return false;
    }
    ++_groups.top().values;
    _current_metadata.emplace(value_metadata{name, description});
    return true;
}

void options_description_building_visitor::visit_value() {
    _groups.top().description.add_options()(_current_metadata->name.c_str(), _current_metadata->description.c_str());
}

void options_description_building_visitor::visit_value(const bool* default_value) {
    describe_value_maybe_default(_groups.top().description, *_current_metadata, default_value);
}

void options_description_building_visitor::visit_value(const int* default_value) {
    describe_value_maybe_default(_groups.top().description, *_current_metadata, default_value);
}

void options_description_building_visitor::visit_value(const unsigned* default_value) {
    auto name = _current_metadata->name;
    if (_current_metadata->name == "smp") {
        name = "smp,c";
    }
    describe_value_maybe_default(_groups.top().description, name, _current_metadata->description, default_value);
}

void options_description_building_visitor::visit_value(const float* default_value) {
    describe_value_maybe_default(_groups.top().description, *_current_metadata, default_value);
}

void options_description_building_visitor::visit_value(const double* default_value) {
    describe_value_maybe_default(_groups.top().description, *_current_metadata, default_value);
}

void options_description_building_visitor::visit_value(const std::string* default_value) {
    auto name = _current_metadata->name;
    if (_current_metadata->name == "memory") {
        name = "memory,m";
    }
    describe_value_maybe_default(_groups.top().description, name, _current_metadata->description, default_value);
}

void options_description_building_visitor::visit_value(const std::set<unsigned>*) {
    describe_value<std::string>(_groups.top().description, *_current_metadata);
}

void options_description_building_visitor::visit_value(const memory::alloc_failure_kind* default_value) {
    describe_enum_value(_groups.top().description, *_current_metadata, default_value);
}

void options_description_building_visitor::visit_value(const log_level* default_value) {
    describe_enum_value(_groups.top().description, *_current_metadata, default_value);
}

void options_description_building_visitor::visit_value(const logger_timestamp_style* default_value) {
    describe_enum_value(_groups.top().description, *_current_metadata, default_value);
}

void options_description_building_visitor::visit_value(const logger_ostream_type* default_value) {
    describe_enum_value(_groups.top().description, *_current_metadata, default_value);
}

void options_description_building_visitor::visit_value(const std::unordered_map<sstring, log_level>*) {
    describe_value<std::vector<std::string>>(_groups.top().description, *_current_metadata);
}

void options_description_building_visitor::visit_selection_value(const std::vector<std::string>& candidates, const std::size_t* selected_candidate) {
    if (selected_candidate) {
        describe_value<std::string>(_groups.top().description, *_current_metadata, candidates.at(*selected_candidate));
    } else {
        describe_value<std::string>(_groups.top().description, *_current_metadata);
    }
}

variables_map_extracting_visitor::variables_map_extracting_visitor(const bpo::variables_map& values) : _values(values) {
}

bool variables_map_extracting_visitor::visit_group_start(const std::string& name, bool used) {
    return used;
}

void variables_map_extracting_visitor::visit_group_end() {
}

bool variables_map_extracting_visitor::visit_value_metadata(const std::string& name, bool used) {
    if (used) {
        _current_name = &name;
        return true;
    } else {
        _current_name = nullptr;
        return false;
    }
}

bool variables_map_extracting_visitor::visit_value() {
    return _values.count(*_current_name);
}

bool variables_map_extracting_visitor::visit_value(bool& val) {
    return extract_value(_values, *_current_name, val);
}

bool variables_map_extracting_visitor::visit_value(int& val) {
    return extract_value(_values, *_current_name, val);
}

bool variables_map_extracting_visitor::visit_value(unsigned& val) {
    return extract_value(_values, *_current_name, val);
}

bool variables_map_extracting_visitor::visit_value(float& val) {
    return extract_value(_values, *_current_name, val);
}

bool variables_map_extracting_visitor::visit_value(double& val) {
    return extract_value(_values, *_current_name, val);
}

bool variables_map_extracting_visitor::visit_value(std::string& val) {
    return extract_value(_values, *_current_name, val);
}

bool variables_map_extracting_visitor::visit_value(std::set<unsigned>& val) {
    std::string raw_val;
    if (!extract_value(_values, *_current_name, raw_val)) {
        return false;
    }
    if (auto parsed_cpu_set = resource::parse_cpuset(raw_val)) {
        val = std::move(*parsed_cpu_set);
        return true;
    }
    throw std::invalid_argument(fmt::format("invalid value for option {}: failed to parse cpuset: {}", *_current_name, raw_val));
}

bool variables_map_extracting_visitor::visit_value(log_level& val) {
    return extract_enum_value(_values, *_current_name, val);
}

bool variables_map_extracting_visitor::visit_value(logger_timestamp_style& val) {
    return extract_enum_value(_values, *_current_name, val);
}

bool variables_map_extracting_visitor::visit_value(logger_ostream_type& val) {
    return extract_enum_value(_values, *_current_name, val);
}

bool variables_map_extracting_visitor::visit_value(memory::alloc_failure_kind& val) {
    return extract_enum_value(_values, *_current_name, val);
}

bool variables_map_extracting_visitor::visit_value(std::unordered_map<sstring, log_level>& val) {
    std::vector<std::string> raw_val;
    if (!extract_value(_values, *_current_name, raw_val)) {
        return false;
    }
    for (const auto& e : raw_val) {
        log_cli::parse_map_associations(e, [&val] (std::string k, std::string v) { val[std::move(k)] = log_cli::parse_log_level(v); });
    }
    return !val.empty();
}

bool variables_map_extracting_visitor::visit_selection_value(const std::vector<std::string>& candidates, std::size_t& selected_candidate) {
    std::string candidate_name;
    if (!extract_value(_values, *_current_name, candidate_name)) {
        return false;
    }
    auto it = std::find(candidates.begin(), candidates.end(), candidate_name);
    if (it == candidates.end()) {
        throw std::invalid_argument(fmt::format("invalid value for option {}: selected candidate doesn't exist: {}", *_current_name, candidate_name));
    }
    selected_candidate = it - candidates.begin();
    return true;
}

} // namespace seastar::program_options
