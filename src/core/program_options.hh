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

#include <boost/program_options.hpp>
#include <seastar/util/program-options.hh>

#include <stack>

namespace bpo = boost::program_options;

namespace seastar::program_options {

/// \cond internal

class options_description_building_visitor : public options_descriptor {
public:
    struct group_metadata {
        const std::string& name;
        bpo::options_description description;
        bool used;
        size_t values = 0;
    };
    struct value_metadata {
        const std::string& name;
        const std::string& description;
    };

private:
    std::stack<group_metadata> _groups;
    std::optional<value_metadata> _current_metadata;

public:
    virtual bool visit_group_start(const std::string& name, bool used) override;
    virtual void visit_group_end() override;

    virtual bool visit_value_metadata(const std::string& name, const std::string& description, bool used) override;

    virtual void visit_value() override;
    virtual void visit_value(const bool* default_value) override;
    virtual void visit_value(const int* default_value) override;
    virtual void visit_value(const unsigned* default_value) override;
    virtual void visit_value(const float* default_value) override;
    virtual void visit_value(const double* default_value) override;
    virtual void visit_value(const std::string* default_value) override;
    virtual void visit_value(const std::set<unsigned>*) override;
    virtual void visit_value(const memory::alloc_failure_kind* default_value) override;
    virtual void visit_value(const log_level* default_value) override;
    virtual void visit_value(const logger_timestamp_style* default_value) override;
    virtual void visit_value(const logger_ostream_type* default_value) override;
    virtual void visit_value(const std::unordered_map<sstring, log_level>*) override;
    virtual void visit_selection_value(const std::vector<std::string>&, const std::size_t*) override;

    bpo::options_description get_options_description() && { return std::move(_groups.top().description); }
};

class variables_map_extracting_visitor : public options_mutator {
    const bpo::variables_map& _values;
    const std::string* _current_name = nullptr;
public:
    explicit variables_map_extracting_visitor(const bpo::variables_map& values);

    virtual bool visit_group_start(const std::string& name, bool used) override;
    virtual void visit_group_end() override;

    virtual bool visit_value_metadata(const std::string& name, bool used) override;

    virtual bool visit_value() override;
    virtual bool visit_value(bool&) override;
    virtual bool visit_value(int&) override;
    virtual bool visit_value(unsigned&) override;
    virtual bool visit_value(float&) override;
    virtual bool visit_value(double&) override;
    virtual bool visit_value(std::string&) override;
    virtual bool visit_value(std::set<unsigned>&) override;
    virtual bool visit_value(log_level&) override;
    virtual bool visit_value(logger_timestamp_style&) override;
    virtual bool visit_value(logger_ostream_type&) override;
    virtual bool visit_value(memory::alloc_failure_kind&) override;
    virtual bool visit_value(std::unordered_map<sstring, log_level>&) override;

    virtual bool visit_selection_value(const std::vector<std::string>&, std::size_t& selected_candidate) override;
};

/// \endcond

} // namespace seastar::program_options
