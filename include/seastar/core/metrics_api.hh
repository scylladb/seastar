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
#include <seastar/util/modules.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/core/sharded.hh>
#ifndef SEASTAR_MODULE
#include <boost/functional/hash.hpp>
#endif

#include <deque>

/*!
 * \file metrics_api.hh
 * \brief header file for metric API layer (like prometheus or collectd)
 *
 *
 *
 */
namespace seastar {
namespace metrics {
namespace impl {

using labels_type = std::map<sstring, sstring>;
using internalized_labels_ref = lw_shared_ptr<const labels_type>;

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

SEASTAR_MODULE_EXPORT
struct relabel_config;

SEASTAR_MODULE_EXPORT
struct metric_family_config;
/*!
 * \brief result of metric relabeling
 *
 * The result of calling set_relabel_configs.
 *
 * metrics_relabeled_due_to_collision the number of metrics that caused conflict
 * and were relabeled to avoid name collision.
 *
 * Non zero value indicates there were name collisions.
 *
 */
SEASTAR_MODULE_EXPORT
struct metric_relabeling_result {
    size_t metrics_relabeled_due_to_collision;
};

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
                    internalized_labels_ref labels)
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
        return _labels->at(shard_label.name());
    }
    const metric_name_type & name() const {
        return _name;
    }
    const labels_type& labels() const {
        return *_labels;
    }
    internalized_labels_ref internalized_labels() const {
        return _labels;
    }
    void update_labels(internalized_labels_ref labels) {
        _labels = labels;
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
    internalized_labels_ref _labels;
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
    internalized_labels_ref original_labels;
    bool enabled;
    skip_when_empty should_skip_when_empty;
};

class internalized_holder {
    internalized_labels_ref _labels;
public:
    explicit internalized_holder(labels_type labels) : _labels(make_lw_shared<labels_type>(std::move(labels))) {
    }

    explicit internalized_holder(internalized_labels_ref labels) : _labels(std::move(labels)) {
    }

    internalized_labels_ref labels_ref() const {
        return _labels;
    }

    const labels_type& labels() const {
        return *_labels;
    }

    size_t has_users() const {
        // Getting the count wrong isn't a correctness issue but will just make internalization worse
        return _labels.use_count() > 1;
    }
};

inline bool operator<(const internalized_holder& lhs, const labels_type& rhs) {
    return lhs.labels() < rhs;
}
inline bool operator<(const labels_type& lhs, const internalized_holder& rhs) {
    return lhs < rhs.labels();
}
inline bool operator<(const internalized_holder& lhs, const internalized_holder& rhs) {
    return lhs.labels() < rhs.labels();
}


class impl;

class registered_metric final {
    metric_info _info;
    metric_function _f;
public:
    registered_metric(metric_id id, metric_function f, bool enabled=true, skip_when_empty skip=skip_when_empty::no);
    metric_value operator()() const {
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
    metric_info& info() {
        return _info;
   }
    const metric_function& get_function() const {
        return _f;
    }
};

using register_ref = shared_ptr<registered_metric>;
using metric_instances = std::map<internalized_holder, register_ref, std::less<>>;
using metrics_registration = std::vector<register_ref>;

class metric_groups_impl : public metric_groups_def {
    metrics_registration _registration;
    shared_ptr<impl> _impl; // keep impl alive while metrics are registered
public:
    metric_groups_impl();
    ~metric_groups_impl();
    metric_groups_impl(const metric_groups_impl&) = delete;
    metric_groups_impl(metric_groups_impl&&) = default;
    metric_groups_impl& add_metric(group_name_type name, const metric_definition& md);
    metric_groups_impl& add_group(group_name_type name, const std::initializer_list<metric_definition>& l);
    metric_groups_impl& add_group(group_name_type name, const std::vector<metric_definition>& l);
};

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

    register_ref& operator[](const internalized_labels_ref& l) {
        return _instances[internalized_holder(l)];
    }

    const register_ref& at(const internalized_labels_ref& l) const {
        return _instances.at(internalized_holder(l));
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

/*!
 * \brief Subset of the per series metadata that is shared via get_values to other shards.
 *
 * Allows omitting metadata that is already stored elsewhere or not needed by
 * the metrics scrap handlers.
 *
 * Not copyable to allow for safely sharing internalized data.
 */
class metric_series_metadata {
    // prom backend only needs the label from here but scollectd needs group and
    // metric name separately. metric_family_info only stores the merged and
    // filtered name so we have to duplicate it here.
    metric_id _id;
    skip_when_empty _should_skip_when_empty;
public:
    metric_series_metadata() = default;
    metric_series_metadata(metric_id id, skip_when_empty should_skip_when_empty)
        : _id(std::move(id)), _should_skip_when_empty(should_skip_when_empty) {
    }

    metric_series_metadata(const metric_series_metadata&) = delete;
    metric_series_metadata& operator=(const metric_series_metadata&) = delete;

    metric_series_metadata(metric_series_metadata&&) noexcept = default;
    metric_series_metadata& operator=(metric_series_metadata&&) noexcept = default;

    const labels_type& labels() const {
      return _id.labels();
    }

    skip_when_empty should_skip_when_empty() const {
        return _should_skip_when_empty;
    }

    group_name_type group_name() const {
        return _id.group_name();
    }

    group_name_type name() const {
        return _id.name();
    }
};

using metric_metadata_fifo = std::deque<metric_series_metadata>;

/*!
 * \brief holds a metric family metadata
 *
 * The meta data of a metric family compose of the
 * metadata of the family, and a vector of the metadata for
 * each of the metrics.
 *
 * The struct is used for two purposes. First, it allows iterating over all metric_families
 * and all metrics related to them. Second, it only contains enabled metrics,
 * making disabled metrics more efficient.
 * The struct is recreated when impl._value_map changes
 */
struct metric_family_metadata {
    metric_family_info mf;
    metric_metadata_fifo metrics;

    metric_family_metadata() = default;
    metric_family_metadata(metric_family_metadata &&) = default;
    metric_family_metadata &operator=(metric_family_metadata &&) = default;
    metric_family_metadata(metric_family_info mf, metric_metadata_fifo metrics)
        : mf(std::move(mf)), metrics(std::move(metrics)) {}
};

static_assert(std::is_nothrow_move_assignable_v<metric_family_metadata>);

using value_vector = std::deque<metric_value>;
using metric_metadata = std::vector<metric_family_metadata>;
using metric_values = std::deque<value_vector>;

struct values_copy {
    shared_ptr<metric_metadata> metadata;
    metric_values values;
};

struct config {
    sstring hostname;
};

using internalized_set = std::set<internalized_holder, std::less<>>;

class impl {
    value_map _value_map;
    config _config;
    bool _dirty = true;
    shared_ptr<metric_metadata> _metadata;
    std::set<sstring> _labels;
    std::vector<std::deque<metric_function>> _current_metrics;
    std::vector<relabel_config> _relabel_configs;
    std::vector<metric_family_config> _metric_family_configs;
    internalized_set _internalized_labels;
public:
    value_map& get_value_map() {
        return _value_map;
    }

    const value_map& get_value_map() const {
        return _value_map;
    }

    register_ref add_registration(const metric_id& id, const metric_type& type, metric_function f, const description& d, bool enabled, skip_when_empty skip, const std::vector<std::string>& aggregate_labels);
    internalized_labels_ref internalize_labels(labels_type labels);
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

    std::vector<std::deque<metric_function>>& functions();

    void update_metrics_if_needed();

    void dirty() {
        _dirty = true;
    }

    const std::set<sstring>& get_labels() const noexcept {
        return _labels;
    }

    future<metric_relabeling_result> set_relabel_configs(const std::vector<relabel_config>& relabel_configs);

    const std::vector<relabel_config>& get_relabel_configs() const noexcept {
        return _relabel_configs;
    }
    const std::vector<metric_family_config>& get_metric_family_configs() const noexcept {
        return _metric_family_configs;
    }

    void set_metric_family_configs(const std::vector<metric_family_config>& metrics_config);

    void update_aggregate(metric_family_info& mf) const noexcept;

private:
    void gc_internalized_labels();
    bool apply_relabeling(const relabel_config& rc, metric_info& info);
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

/*!
 * \brief Perform relabeling and operation on metrics dynamically.
 *
 * The function would return true if the changes were applied with no conflict
 * or false, if there was a conflict in the registration.
 *
  * The general logic follows Prometheus metrics_relabel_config configuration.
 * The relabel rules are applied one after the other.
 * You can add or change a label. you can enable or disable a metric,
 * in that case the metrics will not be reported at all.
 * You can turn on and off the skip_when_empty flag.
 *
 * Using the Prometheus convention, the metric name is __name__.
 * Names cannot be changed.
 *
 * Import notes:
 * - The relabeling always starts from the original set of labels the metric
 *   was created with.
 * - calling with an empty set will remove the relabel config and will
 *   return all metrics to their original labels
 * - To prevent a situation that calling this function would crash the system.
 *   in a situation where a conflicting metrics name are entered, an additional label
 *   will be added to the labels with a unique ID.
 *
 * A few examples:
 * To add a level label with a value 1, to the reactor_utilization metric:
 *  std::vector<sm::relabel_config> rl(1);
    rl[0].source_labels = {"__name__"};
    rl[0].target_label = "level";
    rl[0].replacement = "1";
    rl[0].expr = "reactor_utilization";
   set_relabel_configs(rl);
 *
 * To report only the metrics with the level label equals 1
 *
    std::vector<sm::relabel_config> rl(2);
    rl[0].source_labels = {"__name__"};
    rl[0].action = sm::relabel_config::relabel_action::drop;

    rl[1].source_labels = {"level"};
    rl[1].expr = "1";
    rl[1].action = sm::relabel_config::relabel_action::keep;
    set_relabel_configs(rl);

 */
future<metric_relabeling_result> set_relabel_configs(const std::vector<relabel_config>& relabel_configs);
/*
 * \brief return the current relabel_configs
 * This function returns a vector of the current relabel configs
 */
const std::vector<relabel_config>& get_relabel_configs();

/*
 * \brief change the metrics family config
 *
 * Family config is a configuration that relates to all metrics with the same name but with different labels.
 * set_metric_family_configs allows changing that configuration during run time.
 * Specifically, change the label aggregation based on a metric name.
 *
 * The following is an example for setting the aggregate labels for the metric test_gauge_1
 * and all metrics matching the regex test_gauge1.*:
 *
 * std::vector<sm::metric_family_config> fc(2);
 * fc[0].name = "test_gauge_1";
 * fc[0].aggregate_labels = { "lb" };
 * fc[1].regex_name = "test_gauge1.*";
 * fc[1].aggregate_labels = { "ll", "aa" };
 * sm::set_metric_family_configs(fc);
 */
void set_metric_family_configs(const std::vector<metric_family_config>& metrics_config);

/*
 * \brief return the current metric_family_config
 * This function returns a vector of the current metrics family config
 */
const std::vector<metric_family_config>& get_metric_family_configs();
}
}
