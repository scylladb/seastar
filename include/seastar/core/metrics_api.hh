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
 * Copyright (C) 2016 ScyllaDB.
 */

#pragma once

#include <seastar/core/metrics.hh>
#include <unordered_map>
#include <seastar/core/sharded.hh>
#include <boost/functional/hash.hpp>
/*!
 * \file metrics_api.hh
 * \brief header file for metric API layer (like promehteus or collectd)
 *
 *
 *
 */
namespace seastar {
namespace metrics {
namespace impl {

using labels_type = std::map<sstring, sstring>;
}
}
}

namespace std {

template<>
struct hash<seastar::metrics::impl::labels_type> {
    using argument_type = seastar::metrics::impl::labels_type;
    using result_type = ::std::size_t;
    result_type operator()(argument_type const& s) const {
        result_type h = 0;
        for (auto&& i : s) {
            boost::hash_combine(h, std::hash<seastar::sstring>{}(i.second));
        }
        return h;
    }
};

}

namespace seastar {
namespace metrics {
namespace impl {

/**
 * Metrics are collected in groups that belongs to some logical entity.
 * For example, different measurements of the cpu, will belong to group "cpu".
 *
 * Name is the metric name like used_objects or used_bytes
 *
 * Inherit type allows customizing one of the basic types (gauge, counter, derive).
 *
 * Instance_id is used to differentiate multiple instance of the metrics.
 * In the seastar environment it is typical to have a metric per shard.
 *
 */

class metric_id {
public:
    metric_id() = default;
    metric_id(group_name_type group, metric_name_type name,
                    labels_type labels = {})
                    : _group(std::move(group)), _name(
                                    std::move(name)), _labels(labels) {
    }
    metric_id(metric_id &&) = default;
    metric_id(const metric_id &) = default;

    metric_id & operator=(metric_id &&) = default;
    metric_id & operator=(const metric_id &) = default;

    const group_name_type & group_name() const {
        return _group;
    }
    void group_name(const group_name_type & name) {
        _group = name;
    }
    const instance_id_type & instance_id() const {
        return _labels.at(shard_label.name());
    }
    const metric_name_type & name() const {
        return _name;
    }
    const labels_type& labels() const {
        return _labels;
    }
    sstring full_name() const;

    bool operator<(const metric_id&) const;
    bool operator==(const metric_id&) const;
private:
    auto as_tuple() const {
        return std::tie(group_name(), instance_id(), name(), labels());
    }
    group_name_type _group;
    metric_name_type _name;
    labels_type _labels;
};
}
}
}

namespace std {

template<>
struct hash<seastar::metrics::impl::metric_id>
{
    typedef seastar::metrics::impl::metric_id argument_type;
    typedef ::std::size_t result_type;
    result_type operator()(argument_type const& s) const
    {
        result_type const h1 ( std::hash<seastar::sstring>{}(s.group_name()) );
        result_type const h2 ( std::hash<seastar::sstring>{}(s.instance_id()) );
        return h1 ^ (h2 << 1); // or use boost::hash_combine
    }
};

}

namespace seastar {
namespace metrics {
namespace impl {

/*!
 * \brief holds metadata information of a metric family
 *
 * Holds the information that is shared between all metrics
 * that belongs to the same metric_family
 */
struct metric_family_info {
    data_type type;
    metric_type_def inherit_type;
    description d;
    sstring name;
    std::vector<std::string> aggregate_labels;
};


/*!
 * \brief holds metric metadata
 */
struct metric_info {
    metric_id id;
    bool enabled;
    skip_when_empty should_skip_when_empty;
};


using metrics_registration = std::vector<metric_id>;

class metric_groups_impl : public metric_groups_def {
    metrics_registration _registration;
public:
    metric_groups_impl() = default;
    ~metric_groups_impl();
    metric_groups_impl(const metric_groups_impl&) = delete;
    metric_groups_impl(metric_groups_impl&&) = default;
    metric_groups_impl& add_metric(group_name_type name, const metric_definition& md);
    metric_groups_impl& add_group(group_name_type name, const std::initializer_list<metric_definition>& l);
    metric_groups_impl& add_group(group_name_type name, const std::vector<metric_definition>& l);
};

class impl;

class registered_metric {
    metric_info _info;
    metric_function _f;
    shared_ptr<impl> _impl;
public:
    registered_metric(metric_id id, metric_function f, bool enabled=true, skip_when_empty skip=skip_when_empty::no);
    virtual ~registered_metric() {}
    virtual metric_value operator()() const {
        return _f();
    }

    bool is_enabled() const {
        return _info.enabled;
    }

    void set_enabled(bool b) {
        _info.enabled = b;
    }
    void set_skip_when_empty(skip_when_empty skip) noexcept {
        _info.should_skip_when_empty = skip;
    }
    const metric_id& get_id() const {
        return _info.id;
    }

    const metric_info& info() const {
        return _info;
    }
    metric_function& get_function() {
        return _f;
    }
};

using register_ref = shared_ptr<registered_metric>;
using metric_instances = std::map<labels_type, register_ref>;

class metric_family {
    metric_instances _instances;
    metric_family_info _info;
public:
    using iterator = metric_instances::iterator;
    using const_iterator = metric_instances::const_iterator;

    metric_family() = default;
    metric_family(const metric_family&) = default;
    metric_family(const metric_instances& instances) : _instances(instances) {
    }
    metric_family(const metric_instances& instances, const metric_family_info& info) : _instances(instances), _info(info) {
    }
    metric_family(metric_instances&& instances, metric_family_info&& info) : _instances(std::move(instances)), _info(std::move(info)) {
    }
    metric_family(metric_instances&& instances) : _instances(std::move(instances)) {
    }

    register_ref& operator[](const labels_type& l) {
        return _instances[l];
    }

    const register_ref& at(const labels_type& l) const {
        return _instances.at(l);
    }

    metric_family_info& info() {
        return _info;
    }

    const metric_family_info& info() const {
        return _info;
    }

    iterator find(const labels_type& l) {
        return _instances.find(l);
    }

    const_iterator find(const labels_type& l) const {
        return _instances.find(l);
    }

    iterator begin() {
        return _instances.begin();
    }

    const_iterator begin() const {
        return _instances.cbegin();
    }

    iterator end() {
        return _instances.end();
    }

    bool empty() const {
        return _instances.empty();
    }

    iterator erase(const_iterator position) {
        return _instances.erase(position);
    }

    const_iterator end() const {
        return _instances.cend();
    }

    uint32_t size() const {
        return _instances.size();
    }

};

using value_map = std::map<sstring, metric_family>;

using metric_metadata_vector = std::vector<metric_info>;

/*!
 * \brief holds a metric family metadata
 *
 * The meta data of a metric family compose of the
 * metadata of the family, and a vector of the metadata for
 * each of the metric.
 */
struct metric_family_metadata {
    metric_family_info mf;
    metric_metadata_vector metrics;
};

using value_vector = std::vector<metric_value>;
using metric_metadata = std::vector<metric_family_metadata>;
using metric_values = std::vector<value_vector>;

struct values_copy {
    shared_ptr<metric_metadata> metadata;
    metric_values values;
};

struct config {
    sstring hostname;
};

class impl {
    value_map _value_map;
    config _config;
    bool _dirty = true;
    shared_ptr<metric_metadata> _metadata;
    std::set<sstring> _labels;
    std::vector<std::vector<metric_function>> _current_metrics;
public:
    value_map& get_value_map() {
        return _value_map;
    }

    const value_map& get_value_map() const {
        return _value_map;
    }

    void add_registration(const metric_id& id, const metric_type& type, metric_function f, const description& d, bool enabled, skip_when_empty skip, const std::vector<std::string>& aggregate_labels);
    void remove_registration(const metric_id& id);
    future<> stop() {
        return make_ready_future<>();
    }
    const config& get_config() const {
        return _config;
    }
    void set_config(const config& c) {
        _config = c;
    }

    shared_ptr<metric_metadata> metadata();

    std::vector<std::vector<metric_function>>& functions();

    void update_metrics_if_needed();

    void dirty() {
        _dirty = true;
    }

    const std::set<sstring>& get_labels() const noexcept {
        return _labels;
    }
};

const value_map& get_value_map();
using values_reference = shared_ptr<values_copy>;

foreign_ptr<values_reference> get_values();

shared_ptr<impl> get_local_impl();

void unregister_metric(const metric_id & id);

/*!
 * \brief initialize metric group
 *
 * Create a metric_group_def.
 * No need to use it directly.
 */
std::unique_ptr<metric_groups_def> create_metric_groups();

}

/// Metrics configuration options.
struct options : public program_options::option_group {
    /// \brief The hostname used by the metrics.
    ///
    /// If not set, the local hostname will be used.
    program_options::value<std::string> metrics_hostname;

    options(program_options::option_group* parent_group);
};

/*!
 * \brief set the metrics configuration
 */
future<> configure(const options& opts);

}
}
