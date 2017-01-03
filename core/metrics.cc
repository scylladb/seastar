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

#include "metrics.hh"
#include "metrics_api.hh"
#include <boost/range/algorithm.hpp>

namespace seastar {
namespace metrics {

metric_groups::metric_groups() noexcept : _impl(impl::create_metric_groups()) {
}

void metric_groups::clear() {
    _impl = impl::create_metric_groups();
}

metric_groups::metric_groups(std::initializer_list<metric_group_definition> mg) : _impl(impl::create_metric_groups()) {
    for (auto&& i : mg) {
        add_group(i.name, i.metrics);
    }
}
metric_groups& metric_groups::add_group(const group_name_type& name, const std::initializer_list<metric_definition>& l) {
    _impl->add_group(name, l);
    return *this;
}
metric_group::metric_group() noexcept = default;
metric_group::~metric_group() = default;
metric_group::metric_group(const group_name_type& name, std::initializer_list<metric_definition> l) {
    add_group(name, l);
}

metric_group_definition::metric_group_definition(const group_name_type& name, std::initializer_list<metric_definition> l) : name(name), metrics(l) {
}

metric_group_definition::~metric_group_definition() = default;

metric_groups::~metric_groups() = default;
metric_definition::metric_definition(metric_definition&& m) noexcept : _impl(std::move(m._impl)) {
}

metric_definition::~metric_definition()  = default;

metric_definition::metric_definition(impl::metric_definition_impl const& m) noexcept :
    _impl(std::make_unique<impl::metric_definition_impl>(m)) {
}

bool label_instance::operator<(const label_instance& id2) const {
    auto& id1 = *this;
    return std::tie(id1.key(), id1.value())
                < std::tie(id2.key(), id2.value());
}

bool label_instance::operator==(const label_instance& id2) const {
    auto& id1 = *this;
    return std::tie(id1.key(), id1.value())
                    == std::tie(id2.key(), id2.value());
}

bool label_instance::operator!=(const label_instance& id2) const {
    auto& id1 = *this;
    return !(id1 == id2);
}

namespace impl {

registered_metric::registered_metric(data_type type, metric_function f, description d, bool enabled) :
        _type(type), _d(d), _enabled(enabled), _f(f), _impl(get_local_impl()) {
}

metric_value metric_value::operator+(const metric_value& c) {
    metric_value res(*this);
    switch (_type) {
    case data_type::GAUGE:
        res.u._d += c.u._d;
        break;
    case data_type::DERIVE:
        res.u._i += c.u._i;
        break;
    default:
        res.u._ui += c.u._ui;
        break;
    }
    return res;
}

std::unique_ptr<metric_groups_def> create_metric_groups() {
    return  std::make_unique<metric_groups_impl>();
}


std::unique_ptr<metric_id> get_id(group_name_type group, instance_id_type instance, metric_name_type name,
        metric_type_def iht) {
    return std::make_unique<metric_id>(group, instance, name, iht);
}

metric_groups_impl::~metric_groups_impl() {
    for (auto i : _registration) {
        unregister_metric(i);
    }
}

metric_groups_impl& metric_groups_impl::add_metric(group_name_type name, const metric_definition& md)  {

    metric_id id(name, md._impl->id, md._impl->name, md._impl->type.type_name, md._impl->labels);

    shared_ptr<registered_metric> rm =
            ::make_shared<registered_metric>(md._impl->type.base_type, md._impl->f, md._impl->d, md._impl->enabled);

    get_local_impl()->add_registration(id, rm);

    _registration.push_back(id);
    return *this;
}

metric_groups_impl& metric_groups_impl::add_group(group_name_type name, const std::vector<metric_definition>& l) {
    for (auto i = l.begin(); i != l.end(); ++i) {
        add_metric(name, *(i->_impl.get()));
    }
    return *this;
}

metric_groups_impl& metric_groups_impl::add_group(group_name_type name, const std::initializer_list<metric_definition>& l) {
    for (auto i = l.begin(); i != l.end(); ++i) {
        add_metric(name, *i);
    }
    return *this;
}

void metric_id::sort_labels() {
    boost::sort(_labels);
}

bool metric_id::operator<(
        const metric_id& id2) const {
    return as_tuple() < id2.as_tuple();
}

bool metric_id::operator==(
        const metric_id & id2) const {
    return as_tuple() < id2.as_tuple();
}

// Unfortunately, metrics_impl can not be shared because it
// need to be available before the first users (reactor) will call it

shared_ptr<impl>  get_local_impl() {
    static thread_local auto the_impl = make_shared<impl>();
    return the_impl;
}

void unregister_metric(const metric_id & id) {
    shared_ptr<impl> map = get_local_impl();
    auto i = map->get_value_map().find(id);
    if (i != map->get_value_map().end()) {
        i->second = nullptr;
    }
}

const value_map& get_value_map() {
    return get_local_impl()->get_value_map();
}

values_copy get_values() {
    values_copy res;

    for (auto i : get_local_impl()->get_value_map()) {
        if (i.second.get() && i.second->is_enabled()) {
            res[i.first] = (*(i.second))();
        }
    }
    return std::move(res);
}


instance_id_type shard() {
    return to_sstring(engine().cpu_id());
}

void impl::add_registration(const metric_id& id, shared_ptr<registered_metric> rm) {
    _value_map[id] = rm;
}


}

const bool metric_disabled = false;


}
}
