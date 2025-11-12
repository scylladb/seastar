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



#include <boost/container_hash/hash_fwd.hpp>
#include <ranges>
#include <seastar/core/metrics.hh>
#include <seastar/core/metrics_api.hh>
#include <string_view>

#include <unordered_map>
#include <vector>
#include <string>

namespace seastar::prometheus {


namespace internal {
// return true if label_name appears in aggr_labels
inline bool is_aggregated(const std::vector<std::string>& aggr_labels, std::string_view label_name) {
    return std::find_if(aggr_labels.begin(), aggr_labels.end(), [&](const std::string_view& lhs) {
        return lhs == label_name;
    }) != aggr_labels.end();
}
}

struct label_key {
    using label_list = std::vector<std::string>;

    label_key() {
        // only the scratch key is created with this constructor, we want to reserve
        // some reasonable amount to avoid a bunch of small allocations for the first
        // key (which is sometimes the only key)
        key.reserve(200);
    }

    label_key(const metrics::impl::labels_type& labels, const label_list& aggr_labels) {
        construct(labels, aggr_labels);
    }

    void construct(const metrics::impl::labels_type& labels, const label_list& aggr_labels) {
        key.clear();
        for (auto& [lkey, lvalue] : labels) {
            std::string_view key_view = lkey;
            if (!internal::is_aggregated(aggr_labels, key_view)) {
                key += key_view;
                key += '\n';
                key += lvalue.value();
                key += '\n';
            }
        }

        hash = std::hash<std::string>{}(key);
    }

    bool operator==(const label_key& o) const noexcept {
        return hash == o.hash && key == o.key;
    }

    std::string key;
    size_t hash = 0;
};

} // namespace seastar::prometheus

namespace std {
template <>
struct hash<seastar::prometheus::label_key> {
    size_t operator()(const seastar::prometheus::label_key& lk) const noexcept {
        return lk.hash;
    }
};
}


namespace seastar::prometheus {

/*!
 * \brief a helper class to aggregate metrics over labels
 *
 * This class sum multiple metrics based on a list of labels.
 * It returns one or more metrics each aggregated by the aggregate_by labels.
 *
 * To use it, you define what labels it should aggregate by and then pass to
 * it metrics with their labels.
 * For example if a metrics has a 'shard' and 'name' labels and you aggregate by 'shard'
 * it would return a map of metrics each with only the 'name' label
 *
 */
class metric_aggregate_by_labels {
    using labels_type = metrics::impl::labels_type;

    struct labels_value {
        labels_type labels;
        metrics::impl::metric_value m;
    };

public:
    using label_list_type = std::vector<std::string>;
    using map_type = std::unordered_map<label_key, labels_value>;
    const label_list_type& _labels_to_aggregate_by;
    label_key scratch_key;
    map_type _values;
public:
    metric_aggregate_by_labels(const label_list_type& labels) : _labels_to_aggregate_by(labels) {
    }
    /*!
     * \brief add a metric
     *
     * This method gets a metric and its labels and adds it to the aggregated metric.
     * For example, if a metric has the labels {'shard':'0', 'name':'myhist'} and we are aggregating
     * over 'shard'
     * The metric would be added to the aggregated metric with labels {'name':'myhist'}.
     *
     */
    void add(const seastar::metrics::impl::metric_value& m, const labels_type& input_labels) noexcept {
        scratch_key.construct(input_labels, _labels_to_aggregate_by);
        auto i = _values.find(scratch_key);
        if (i == _values.end()) {
            labels_type labels;
            for (auto& l : input_labels) {
                if (!internal::is_aggregated(_labels_to_aggregate_by, l.first)) {
                    labels.insert(l);
                }
            }
            _values.emplace(scratch_key, labels_value{std::move(labels), m});
        } else {
            i->second.m += m;
        }
    }
    const auto& get_values() const noexcept {
        return _values;
    }
    bool empty() const noexcept {
        return _values.empty();
    }
};

}
