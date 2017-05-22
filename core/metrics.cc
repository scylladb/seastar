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
#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/range/algorithm_ext/erase.hpp>

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


static std::string get_hostname() {
    char hostname[PATH_MAX];
    gethostname(hostname, sizeof(hostname));
    hostname[PATH_MAX-1] = '\0';
    return hostname;
}


boost::program_options::options_description get_options_description() {
    namespace bpo = boost::program_options;
    bpo::options_description opts("Metrics options");
    opts.add_options()(
            "metrics-hostname",
            bpo::value<std::string>()->default_value(get_hostname()),
            "set the hostname used by the metrics, if not set, the local hostname will be used");
    return opts;
}

future<> configure(const boost::program_options::variables_map & opts) {
    impl::config c;
    c.hostname = opts["metrics-hostname"].as<std::string>();
    return smp::invoke_on_all([c] {
        impl::get_local_impl()->set_config(c);
    });
}


bool label_instance::operator!=(const label_instance& id2) const {
    auto& id1 = *this;
    return !(id1 == id2);
}

label shard_label("shard");
label type_label("type");
namespace impl {

registered_metric::registered_metric(metric_id id, metric_function f, bool enabled) :
        _f(f), _impl(get_local_impl()) {
    _info.enabled = enabled;
    _info.id = id;
}

metric_value metric_value::operator+(const metric_value& c) {
    metric_value res(*this);
    switch (_type) {
    case data_type::HISTOGRAM:
        boost::get<histogram>(res.u) += boost::get<histogram>(c.u);
    default:
        boost::get<double>(res.u) += boost::get<double>(c.u);
        break;
    }
    return res;
}

metric_definition_impl::metric_definition_impl(
        metric_name_type name,
        metric_type type,
        metric_function f,
        description d,
        std::vector<label_instance> _labels)
        : name(name), type(type), f(f)
        , d(d), enabled(true) {
    for (auto i: _labels) {
        labels[i.key()] = i.value();
    }
    if (labels.find(shard_label.name()) == labels.end()) {
        labels[shard_label.name()] = shard();
    }
    if (labels.find(type_label.name()) == labels.end()) {
        labels[type_label.name()] = type.type_name;
    }
}

metric_definition_impl& metric_definition_impl::operator ()(bool _enabled) {
    enabled = _enabled;
    return *this;
}

metric_definition_impl& metric_definition_impl::operator ()(const label_instance& label) {
    labels[label.key()] = label.value();
    return *this;
}

std::unique_ptr<metric_groups_def> create_metric_groups() {
    return  std::make_unique<metric_groups_impl>();
}

metric_groups_impl::~metric_groups_impl() {
    for (auto i : _registration) {
        unregister_metric(i);
    }
}

metric_groups_impl& metric_groups_impl::add_metric(group_name_type name, const metric_definition& md)  {

    metric_id id(name, md._impl->name, md._impl->labels);

    get_local_impl()->add_registration(id, md._impl->type.base_type, md._impl->f, md._impl->d, md._impl->enabled);

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

bool metric_id::operator<(
        const metric_id& id2) const {
    return as_tuple() < id2.as_tuple();
}

static std::string safe_name(const sstring& name) {
    auto rep = boost::replace_all_copy(boost::replace_all_copy(name, "-", "_"), " ", "_");
    boost::remove_erase_if(rep, boost::is_any_of("+()"));
    return rep;
}

sstring metric_id::full_name() const {
    return safe_name(_group + "_" + _name);
}

bool metric_id::operator==(
        const metric_id & id2) const {
    return as_tuple() == id2.as_tuple();
}

// Unfortunately, metrics_impl can not be shared because it
// need to be available before the first users (reactor) will call it

shared_ptr<impl>  get_local_impl() {
    static thread_local auto the_impl = ::seastar::make_shared<impl>();
    return the_impl;
}
void impl::remove_registration(const metric_id& id) {
    auto i = get_value_map().find(id.full_name());
    if (i != get_value_map().end()) {
        auto j = i->second.find(id.labels());
        if (j != i->second.end()) {
            j->second = nullptr;
            i->second.erase(j);
        }
        if (i->second.empty()) {
            get_value_map().erase(i);
        }
        dirty();
    }
}

void unregister_metric(const metric_id & id) {
    get_local_impl()->remove_registration(id);
}

const value_map& get_value_map() {
    return get_local_impl()->get_value_map();
}

foreign_ptr<values_reference> get_values() {
    shared_ptr<values_copy> res_ref = ::seastar::make_shared<values_copy>();
    auto& res = *(res_ref.get());
    auto& mv = res.values;
    res.metadata = get_local_impl()->metadata();
    auto & functions = get_local_impl()->functions();
    mv.reserve(functions.size());
    for (auto&& i : functions) {
        value_vector values;
        values.reserve(i.size());
        for (auto&& v : i) {
            values.emplace_back(v());
        }
        mv.emplace_back(std::move(values));
    }
    return res_ref;
}


instance_id_type shard() {
    if (engine_is_ready()) {
        return to_sstring(engine().cpu_id());
    }

    return sstring("0");
}

void impl::update_metrics_if_needed() {
    if (_dirty) {
        // Forcing the metadata to an empty initialization
        // Will prevent using corrupted data if an exception is thrown
        _metadata = ::seastar::make_shared<metric_metadata>();

        auto mt_ref = ::seastar::make_shared<metric_metadata>();
        auto &mt = *(mt_ref.get());
        mt.reserve(_value_map.size());
        _current_metrics.resize(_value_map.size());
        size_t i = 0;
        for (auto&& mf : _value_map) {
            metric_metadata_vector metrics;
            _current_metrics[i].clear();
            for (auto&& m : mf.second) {
                if (m.second && m.second->is_enabled()) {
                    metrics.emplace_back(m.second->info());
                    _current_metrics[i].emplace_back(m.second->get_function());
                }
            }
            if (!metrics.empty()) {
                // If nothing was added, no need to add the metric_family
                // and no need to progress
                mt.emplace_back(metric_family_metadata{mf.second.info(), std::move(metrics)});
                i++;
            }
        }
        // Maybe we didn't use all the original size
        _current_metrics.resize(i);
        _metadata = mt_ref;
        _dirty = false;
    }
}

shared_ptr<metric_metadata> impl::metadata() {
    update_metrics_if_needed();
    return _metadata;
}

std::vector<std::vector<metric_function>>& impl::functions() {
    update_metrics_if_needed();
    return _current_metrics;
}

void impl::add_registration(const metric_id& id, data_type type, metric_function f, const description& d, bool enabled) {
    auto rm = ::seastar::make_shared<registered_metric>(id, f, enabled);
    sstring name = id.full_name();
    if (_value_map.find(name) != _value_map.end()) {
        auto& metric = _value_map[name];
        if (metric.find(id.labels()) != metric.end()) {
            throw std::runtime_error("registering metrics twice for metrics: " + name);
        }
        if (metric.info().type != type) {
            throw std::runtime_error("registering metrics " + name + " registered with different type.");
        }
        metric[id.labels()] = rm;
    } else {
        _value_map[name].info().type = type;
        _value_map[name].info().d = d;
        _value_map[name].info().name = id.full_name();
        _value_map[name][id.labels()] = rm;
    }
    dirty();
}

}

const bool metric_disabled = false;


histogram& histogram::operator+=(const histogram& c) {
    for (size_t i = 0; i < c.buckets.size(); i++) {
        if (buckets.size() <= i) {
            buckets.push_back(c.buckets[i]);
        } else {
            if (buckets[i].upper_bound != c.buckets[i].upper_bound) {
                throw std::out_of_range("Trying to add histogram with different bucket limits");
            }
            buckets[i].count += c.buckets[i].count;
        }
    }
    return *this;
}

histogram histogram::operator+(const histogram& c) const {
    histogram res = *this;
    res += c;
    return res;
}

histogram histogram::operator+(histogram&& c) const {
    c += *this;
    return std::move(c);
}

}
}
