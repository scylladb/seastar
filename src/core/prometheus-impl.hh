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
        auto k = metrics::impl::build_aggregation_key(labels, aggr_labels);
        key.assign(k.data(), k.size());
        hash = std::hash<std::string_view>{}(std::string_view(key));
    }

    // Fast path: adopt a key/hash that was already computed once, e.g. by
    // metrics::impl::metric_series_metadata when metadata was last rebuilt, instead of
    // re-filtering labels and rebuilding the key string on every scrape.
    void assign(std::string_view precomputed_key, size_t precomputed_hash) {
        key.assign(precomputed_key);
        hash = precomputed_hash;
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
    // identifies this scrape's aggregate_labels config, to detect a per-series cache
    // built under a different (e.g. other shard's, mid-reconfiguration) config.
    size_t _labels_to_aggregate_by_hash;
    label_key scratch_key;
    map_type _values;

    labels_type build_aggregated_labels(const labels_type& input_labels) const {
        labels_type labels;
        for (auto& l : input_labels) {
            if (!internal::is_aggregated(_labels_to_aggregate_by, l.first)) {
                labels.insert(l);
            }
        }
        return labels;
    }
public:
    metric_aggregate_by_labels(const label_list_type& labels)
        : _labels_to_aggregate_by(labels)
        , _labels_to_aggregate_by_hash(metrics::impl::hash_aggregate_labels(labels)) {
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
            _values.emplace(scratch_key, labels_value{build_aggregated_labels(input_labels), m});
        } else {
            i->second.m += m;
        }
    }

    /*!
     * \brief add a metric using a precomputed aggregation key
     *
     * Same as add() above, but uses the aggregation key already cached on value_info (see
     * metrics::impl::metric_series_metadata) instead of rebuilding it from the full label
     * set on every call. The post-aggregation label set is only ever needed once per unique
     * output group, so it's built here on a cache miss rather than cached per input series.
     * This is the path used by the prometheus scrape handlers; the label-based add() above
     * is kept for direct testing of this class and any other caller.
     */
    void add(const seastar::metrics::impl::metric_value& m, const seastar::metrics::impl::metric_series_metadata& value_info) noexcept {
        if (value_info.has_aggregation_cache() && value_info.aggregation_key_config_hash() == _labels_to_aggregate_by_hash) [[likely]] {
            scratch_key.assign(value_info.aggregation_key(), value_info.aggregation_key_hash());
        } else {
            // Rare: either this shard's metadata hasn't picked up an aggregate_labels
            // change yet (no cache), or it rebuilt under a different config than the
            // one driving this scrape (stale cache, cross-shard reconfiguration race).
            // Recompute instead of trusting a cache that isn't there or doesn't match.
            scratch_key.construct(value_info.labels(), _labels_to_aggregate_by);
        }
        auto i = _values.find(scratch_key);
        if (i == _values.end()) {
            _values.emplace(scratch_key, labels_value{build_aggregated_labels(value_info.labels()), m});
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
