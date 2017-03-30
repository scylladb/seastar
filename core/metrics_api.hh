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

#include "metrics.hh"
#include <unordered_map>
#include "sharded.hh"
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
            boost::hash_combine(h, std::hash<sstring>{}(i.second));
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
    const metrics::metric_type_def & inherit_type() const {
        return _labels.at(type_label.name());
    }
    const labels_type& labels() const {
        return _labels;
    }
    sstring full_name() const;

    bool operator<(const metric_id&) const;
    bool operator==(const metric_id&) const;
private:
    auto as_tuple() const {
        return std::tie(group_name(), instance_id(), name(),
                    inherit_type(), labels());
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
        result_type const h1 ( std::hash<sstring>{}(s.group_name()) );
        result_type const h2 ( std::hash<sstring>{}(s.instance_id()) );
        return h1 ^ (h2 << 1); // or use boost::hash_combine
    }
};

}

namespace seastar {
namespace metrics {
namespace impl {

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
    data_type _type;
    description _d;
    bool _enabled;
    metric_function _f;
    shared_ptr<impl> _impl;
    metric_id _id;
public:
    registered_metric(metric_id id, data_type type, metric_function f, description d = description(), bool enabled=true);
    virtual ~registered_metric() {}
    virtual metric_value operator()() const {
        return _f();
    }
    data_type get_type() const {
        return _type;
    }

    bool is_enabled() const {
        return _enabled;
    }

    void set_enabled(bool b) {
        _enabled = b;
    }

    const description& get_description() const {
        return _d;
    }

    const metric_id& get_id() const {
        return _id;
    }
};

/*!
 * \brief holds information that relevant to all metric instances
 */
struct metric_info {
    data_type type;
};



using register_ref = shared_ptr<registered_metric>;
using metric_instances = std::unordered_map<labels_type, register_ref>;

class metric_family {
    metric_instances _instances;
    metric_info _info;
public:
    using iterator = metric_instances::iterator;
    using const_iterator = metric_instances::const_iterator;

    metric_family() = default;
    metric_family(const metric_family&) = default;
    metric_family(const metric_instances& instances) : _instances(instances) {
    }
    metric_family(const metric_instances& instances, const metric_info& info) : _instances(instances), _info(info) {
    }
    metric_family(metric_instances&& instances, metric_info&& info) : _instances(std::move(instances)), _info(std::move(info)) {
    }
    metric_family(metric_instances&& instances) : _instances(std::move(instances)) {
    }

    register_ref& operator[](const labels_type& l) {
        return _instances[l];
    }

    const register_ref& at(const labels_type& l) const {
        return _instances.at(l);
    }

    metric_info& info() {
        return _info;
    }

    const metric_info& info() const {
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

};

using value_map = std::unordered_map<sstring, metric_family>;
using value_holder = std::tuple<register_ref, metric_value>;
using value_vector = std::vector<value_holder>;
using values_copy = std::unordered_map<sstring, value_vector>;

struct config {
    sstring hostname;
};

class impl {
    value_map _value_map;
    config _config;
public:
    value_map& get_value_map() {
        return _value_map;
    }

    const value_map& get_value_map() const {
        return _value_map;
    }

    void add_registration(const metric_id& id, shared_ptr<registered_metric> rm);

    future<> stop() {
        return make_ready_future<>();
    }
    const config& get_config() const {
        return _config;
    }
    void set_config(const config& c) {
        _config = c;
    }
};

const value_map& get_value_map();

values_copy get_values();

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
/*!
 * \brief set the metrics configuration
 */
future<> configure(const boost::program_options::variables_map & opts);

/*!
 * \brief get the metrics configuration desciprtion
 */

boost::program_options::options_description get_options_description();

}
}
