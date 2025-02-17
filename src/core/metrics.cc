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

#ifdef SEASTAR_MODULE
module;
#endif

#include <memory>
#include <regex>
#include <random>
#include <variant>
#include <boost/range/algorithm.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/range/algorithm_ext/erase.hpp>

#ifdef SEASTAR_MODULE
module seastar;
#else
#include <seastar/core/metrics.hh>
#include <seastar/core/metrics_api.hh>
#include <seastar/core/relabel_config.hh>
#include <seastar/core/reactor.hh>
#endif

namespace seastar {
extern seastar::logger seastar_logger;
namespace metrics {

double_registration::double_registration(std::string what): std::runtime_error(what) {}

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
metric_groups& metric_groups::add_group(const group_name_type& name, const std::vector<metric_definition>& l) {
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


options::options(program_options::option_group* parent_group)
    : program_options::option_group(parent_group, "Metrics options")
    , metrics_hostname(*this, "metrics-hostname", get_hostname(),
            "set the hostname used by the metrics, if not set, the local hostname will be used")
{
}

future<> configure(const options& opts) {
    impl::config c;
    c.hostname = opts.metrics_hostname.get_value();
    return smp::invoke_on_all([c] {
        impl::get_local_impl()->set_config(c);
    });
}

future<metric_relabeling_result> set_relabel_configs(const std::vector<relabel_config>& relabel_configs) {
    return impl::get_local_impl()->set_relabel_configs(relabel_configs);
}

const std::vector<relabel_config>& get_relabel_configs() {
    return impl::get_local_impl()->get_relabel_configs();
}
void impl::impl::update_aggregate(metric_family_info& mf) const noexcept {
    for (const auto& fc : _metric_family_configs) {
        if (fc.name == mf.name || fc.regex_name.match(mf.name)) {
            mf.aggregate_labels = fc.aggregate_labels;
        }
    }
}
void set_metric_family_configs(const std::vector<metric_family_config>& family_config) {
    impl::get_local_impl()->set_metric_family_configs(family_config);
}

const std::vector<metric_family_config>& get_metric_family_configs() {
    return impl::get_local_impl()->get_metric_family_configs();
}

bool impl::impl::apply_relabeling(const relabel_config& rc, metric_info& info) {
    std::stringstream s;
    bool first = true;
    for (auto&& l: rc.source_labels) {
        auto val = info.id.labels().find(l);
        if (l != "__name__" && val == info.id.labels().end()) {
            //If not all the labels are found nothing todo
            return false;
        }
        if (first) {
            first = false;
        } else {
            s << rc.separator;
        }
        s << ((l == "__name__") ? info.id.full_name() : val->second);
    }
    std::smatch match;
    // regex_search forbid temporary strings
    std::string tmps = s.str();
    if (!std::regex_search(tmps, match, rc.expr.regex())) {
        return false;
    }

    switch (rc.action) {
        case relabel_config::relabel_action::drop:
        case relabel_config::relabel_action::keep: {
            info.enabled = rc.action == relabel_config::relabel_action::keep;
            return true;
        }
        case relabel_config::relabel_action::report_when_empty:
        case relabel_config::relabel_action::skip_when_empty: {
            info.should_skip_when_empty = (rc.action == relabel_config::relabel_action::skip_when_empty) ? skip_when_empty::yes : skip_when_empty::no;
            return false;
        }
        case relabel_config::relabel_action::drop_label: {
            if (info.id.labels().find(rc.target_label) != info.id.labels().end()) {
                auto new_labels = info.id.labels();
                new_labels.erase(rc.target_label);
                info.id.update_labels(internalize_labels(std::move(new_labels)));
            }
            return true;
        };
        case relabel_config::relabel_action::replace: {
            if (!rc.target_label.empty()) {
                std::string fmt_s = match.format(rc.replacement);
                auto new_labels = info.id.labels();
                new_labels[rc.target_label] = fmt_s;
                info.id.update_labels(internalize_labels(std::move(new_labels)));
            }
            return true;
        }
        default:
            break;
    }
    return true;
}

bool label_instance::operator!=(const label_instance& id2) const {
    auto& id1 = *this;
    return !(id1 == id2);
}

/*!
 * \brief get_unique_id generate a random id
 */
static std::string get_unique_id() {
    std::random_device rd;
    return std::to_string(rd()) + "-" + std::to_string(rd()) + "-" + std::to_string(rd()) + "-" + std::to_string(rd());
}

label shard_label("shard");
namespace impl {

registered_metric::registered_metric(metric_id id, metric_function f, bool enabled, skip_when_empty skip) :
        _f(f) {
    _info.enabled = enabled;
    _info.should_skip_when_empty = skip;
    _info.id = id;
    _info.original_labels = id.internalized_labels();
}

metric_value metric_value::operator+(const metric_value& c) {
    metric_value res(*this);
    switch (_type) {
    case data_type::HISTOGRAM:
    case data_type::SUMMARY:
        std::get<histogram>(res.u) += std::get<histogram>(c.u);
        break;
    default:
        std::get<double>(res.u) += std::get<double>(c.u);
        break;
    }
    return res;
}

void metric_value::ulong_conversion_error(double d) {
    throw std::range_error(format("cannot convert double value {} to unsigned long", d));
}

metric_definition_impl::metric_definition_impl(
        metric_name_type name,
        metric_type type,
        metric_function f,
        description d,
        std::vector<label_instance> _labels,
        std::vector<label> _aggregate_labels)
        : name(name), type(type), f(f)
        , d(d), enabled(true) {
    for (auto i: _labels) {
        labels[i.key()] = i.value();
    }
    if (labels.find(shard_label.name()) == labels.end()) {
        labels[shard_label.name()] = shard();
    }
    aggregate(_aggregate_labels);
}

metric_definition_impl& metric_definition_impl::operator ()(bool _enabled) {
    enabled = _enabled;
    return *this;
}

metric_definition_impl& metric_definition_impl::operator ()(const label_instance& label) {
    labels[label.key()] = label.value();
    return *this;
}

metric_definition_impl& metric_definition_impl::operator ()(skip_when_empty skip) noexcept {
    _skip_when_empty = skip;
    return *this;
}

metric_definition_impl& metric_definition_impl::set_type(const sstring& type_name) {
    type.type_name = type_name;
    return *this;
}

metric_definition_impl& metric_definition_impl::aggregate(const std::vector<label>& _labels) noexcept {
    aggregate_labels.reserve(_labels.size());
    std::transform(_labels.begin(), _labels.end(),std::back_inserter(aggregate_labels),
            [](const label& l) { return l.name(); });
    return *this;
}

metric_definition_impl& metric_definition_impl::set_skip_when_empty(bool skip) noexcept {
    _skip_when_empty = skip_when_empty(skip);
    return *this;
}

std::unique_ptr<metric_groups_def> create_metric_groups() {
    return  std::make_unique<metric_groups_impl>();
}

metric_groups_impl::metric_groups_impl() {}

metric_groups_impl::~metric_groups_impl() {
    for (const auto& i : _registration) {
        unregister_metric(i->info().id);
    }
}

internalized_labels_ref impl::internalize_labels(labels_type labels) {
    auto it = _internalized_labels.find(labels);
    if (it == _internalized_labels.end()) {
        it = _internalized_labels.emplace(labels).first;
    }
    return it->labels_ref();
}

metric_groups_impl& metric_groups_impl::add_metric(group_name_type name, const metric_definition& md)  {
    // We could just do this in the constructor but some metric groups (like the
    // smp queue ones) are actually constructed on a different shard originally
    // than where the actual metrics are added.
    // Hence, the shared_ptr owning shard check would fail so we do it only here.
    if (_impl == nullptr) {
        _impl = get_local_impl();
    }

    auto internalized_labels = get_local_impl()->internalize_labels(md._impl->labels);

    metric_id id(name, md._impl->name, internalized_labels);

    auto reg = get_local_impl()->add_registration(id, md._impl->type, md._impl->f, md._impl->d, md._impl->enabled, md._impl->_skip_when_empty, md._impl->aggregate_labels);

    _registration.push_back(std::move(reg));
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
    for (auto&& i : functions) {
        value_vector values;
        for (auto&& v : i) {
            values.emplace_back(v());
        }
        mv.emplace_back(std::move(values));
    }
    return res_ref;
}


instance_id_type shard() {
    return to_sstring(this_shard_id());
}

void impl::gc_internalized_labels() {
    for (auto it = _internalized_labels.begin(); it != _internalized_labels.end();) {
        if (!it->has_users()) {
            it = _internalized_labels.erase(it);
        } else {
            ++it;
        }
    }
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
            metric_metadata_fifo metrics;
            _current_metrics[i].clear();
            for (auto&& m : mf.second) {
                if (m.second && m.second->is_enabled()) {
                    metrics.emplace_back(m.second->info().id, m.second->info().should_skip_when_empty);
                    _current_metrics[i].emplace_back(m.second->get_function());
                }
            }
            if (!metrics.empty()) {
                // If nothing was added, no need to add the metric_family
                // and no need to progress
                mt.emplace_back(mf.second.info(), std::move(metrics));
                i++;
            }
        }
        // Maybe we didn't use all the original size
        _current_metrics.resize(i);
        _metadata = mt_ref;
        _dirty = false;

        gc_internalized_labels();
    }
}

shared_ptr<metric_metadata> impl::metadata() {
    update_metrics_if_needed();
    return _metadata;
}

std::vector<std::deque<metric_function>>& impl::functions() {
    update_metrics_if_needed();
    return _current_metrics;
}

register_ref impl::add_registration(const metric_id& id, const metric_type& type, metric_function f, const description& d, bool enabled, skip_when_empty skip, const std::vector<std::string>& aggregate_labels) {
    auto rm = ::seastar::make_shared<registered_metric>(id, f, enabled, skip);
    for (auto&& rl : _relabel_configs) {
        apply_relabeling(rl, rm->info());
    }

    sstring name = id.full_name();
    if (_value_map.find(name) != _value_map.end()) {
        auto& metric = _value_map[name];
        if (metric.find(rm->info().id.labels()) != metric.end()) {
            throw double_registration("registering metrics twice for metrics: " + name);
        }
        if (metric.info().type != type.base_type) {
            throw std::runtime_error("registering metrics " + name + " registered with different type.");
        }
        metric[rm->info().id.internalized_labels()] = rm;
        for (auto&& i : rm->info().id.labels()) {
            _labels.insert(i.first);
        }
    } else {
        _value_map[name].info().type = type.base_type;
        _value_map[name].info().d = d;
        _value_map[name].info().inherit_type = type.type_name;
        _value_map[name].info().name = id.full_name();
        _value_map[name].info().aggregate_labels = aggregate_labels;
        impl::update_aggregate(_value_map[name].info());
        _value_map[name][rm->info().id.internalized_labels()] = rm;
    }
    dirty();

    return rm;
}

future<metric_relabeling_result> impl::set_relabel_configs(const std::vector<relabel_config>& relabel_configs) {
    _relabel_configs = relabel_configs;
    metric_relabeling_result conflicts{0};
    for (auto&& family : _value_map) {
        std::vector<shared_ptr<registered_metric>> rms;
        for (auto&& metric = family.second.begin(); metric != family.second.end();) {
            metric->second->info().id.update_labels(metric->second->info().original_labels);
            for (auto rl : _relabel_configs) {
                if (apply_relabeling(rl, metric->second->info())) {
                    dirty();
                }
            }
            if (metric->first.labels() != metric->second->info().id.labels()) {
                // If a metric labels were changed, we should remove it from the map, and place it back again
                rms.push_back(metric->second);
                family.second.erase(metric++);
                dirty();
            } else {
                ++metric;
            }
        }
        for (auto rm : rms) {
            auto ilb = rm->info().id.internalized_labels();
            if (family.second.find(rm->info().id.labels()) != family.second.end()) {
                /*
                 You can not have a two metrics with the same name and label, there is a problem with the configuration.
                 On startup we would have throw an exception and stop.
                 But during normal run, we don't want to crash the server just because a metric reconfiguration.
                 We cannot throw away the metric.
                 Instead we add it with an extra label, allowing the user to reconfigure.
                */
                seastar_logger.error("Metrics: After relabeling, registering metrics twice for metrics : {}", family.first);
                auto id = get_unique_id();
                auto new_labels = *ilb;
                new_labels["err"] = id;
                ilb = internalize_labels(new_labels);
                conflicts.metrics_relabeled_due_to_collision++;
                rm->info().id.update_labels(ilb);
            }

            family.second[ilb] = rm;
        }
    }
    return make_ready_future<metric_relabeling_result>(conflicts);
}

void impl::set_metric_family_configs(const std::vector<metric_family_config>& family_config) {
    _metric_family_configs = family_config;
    for (auto& [name, family] : _value_map) {
        for  (const auto& fc : family_config) {
            if (fc.name == name || fc.regex_name.match(name)) {
                family.info().aggregate_labels = fc.aggregate_labels;
            }
        }
    }

    if (!_metadata) {
        // The metadata structure may not have been built yet,
        // or it may rebuild right now, in this case just return
        // and it will be updated the next time, setting the dirty to true
        // is just in case we are somehow in the middle of rebuilding the
        // metadata.
        dirty();
        return;
    }
    for (auto& mf_metadata: *_metadata) {
        for  (const auto& fc : family_config) {
            if (fc.name == mf_metadata.mf.name || fc.regex_name.match(mf_metadata.mf.name)) {
                mf_metadata.mf.aggregate_labels = fc.aggregate_labels;
            }
        }
    }
}
}

const bool metric_disabled = false;

relabel_config::relabel_action relabel_config_action(const std::string& action) {
    if (action == "skip_when_empty") {
        return relabel_config::relabel_action::skip_when_empty;
    }
    if (action == "report_when_empty") {
        return relabel_config::relabel_action::report_when_empty;
    }
    if (action == "keep") {
        return relabel_config::relabel_action::keep;
    }
    if (action == "drop") {
        return relabel_config::relabel_action::drop;
    } if (action == "drop_label") {
        return relabel_config::relabel_action::drop_label;
    }
    return relabel_config::relabel_action::replace;
}

histogram& histogram::operator+=(const histogram& c) {
    if (c.sample_count == 0) {
        return *this;
    }
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
    sample_count += c.sample_count;
    sample_sum += c.sample_sum;
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
