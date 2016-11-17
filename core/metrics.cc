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

namespace seastar {
namespace metrics {

namespace impl {
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

std::unique_ptr<metric_id> get_id(group_name_type group, instance_id_type instance, measurement_type measurement,
        metrics::sub_measurement_type sm) {
    return std::make_unique<metric_id>(group, instance, measurement, sm);
}

metric_groups_impl::~metric_groups_impl() {
    for (auto i : _registration) {
        unregister_metric(i);
    }
}

metric_groups_impl& metric_groups_impl::add_metric(group_name_type name, const metric_definition& md)  {

    metric_id id(name, md.id, md.mt, md.smt);

    shared_ptr<registered_metric> rm =
            ::make_shared<registered_metric>(md.dt, md.f, md.d, md.enabled);

    get_local_impl().add_registration(id, rm);

    _registration.push_back(id);
    return *this;
}

metric_groups_impl& metric_groups_impl::add_group(group_name_type name, const std::vector<metric_definition>& l) {
    for (auto i = l.begin(); i != l.end(); ++i) {
        add_metric(name, *i);
    }
    return *this;
}

metric_groups_impl& metric_groups_impl::add_group(group_name_type name, const std::initializer_list<metric_definition>& l) {
    for (auto i = l.begin(); i != l.end(); ++i) {
        add_metric(name, *i);
    }
    return *this;
}

bool metric_id::operator<(
        const metric_id& id2) const {
    auto& id1 = *this;
    return std::tie(id1.group_name(), id1.instance_id(), id1.measurement(),
            id1.sub_measurement())
            < std::tie(id2.group_name(), id2.instance_id(), id2.measurement(),
                    id2.sub_measurement());
}

bool metric_id::operator==(
        const metric_id & id2) const {
    auto& id1 = *this;
    return std::tie(id1.group_name(), id1.instance_id(), id1.measurement(),
            id1.sub_measurement())
            == std::tie(id2.group_name(), id2.instance_id(), id2.measurement(),
                    id2.sub_measurement());
}

// Unfortunately, metrics_impl can not be shared because it
// need to be available before the first users (reactor) will call it
thread_local impl metrics_impl;

impl& get_local_impl() {
    return metrics_impl;
}

void unregister_metric(const metric_id & id) {
    value_map& map = metrics_impl.get_value_map();
    auto i = map.find(id);
    if (i != map.end()) {
        i->second = nullptr;
    }
}

const value_map& get_value_map() {
    return metrics_impl.get_value_map();
}

values_copy get_values() {
    values_copy res;

    for (auto i : metrics_impl.get_value_map()) {
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


const bool metric_disabled = false;
}

metric_groups create_metric_groups() {
    metric_groups res =  std::make_unique<impl::metric_groups_impl>();
    return std::move(res);
}

}
}
