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

    label_key(std::string key, size_t hash) : key(std::move(key)), hash(hash) {}

    void construct(const metrics::impl::labels_type& labels, const label_list& aggr_labels) {
        key.clear();
        metrics::impl::append_aggregation_key(key, labels, aggr_labels);
        hash = std::hash<std::string_view>{}(std::string_view(key));
    }

    std::string key;
    size_t hash = 0;
};

// Non-owning label_key: lets the map be probed with a key already cached on a series
// (metric_series_metadata) without copying it into an owned string first.
struct label_key_view {
    std::string_view key;
    size_t hash;
};

struct label_key_hash {
    using is_transparent = void;
    size_t operator()(const label_key& k) const noexcept { return k.hash; }
    size_t operator()(const label_key_view& k) const noexcept { return k.hash; }
};

struct label_key_equal {
    using is_transparent = void;
    template <typename A, typename B>
    bool operator()(const A& a, const B& b) const noexcept {
        return a.hash == b.hash && std::string_view(a.key) == std::string_view(b.key);
    }
};

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
    using map_type = std::unordered_map<label_key, labels_value, label_key_hash, label_key_equal>;
    const label_list_type& _labels_to_aggregate_by;
    // identifies this scrape's aggregate_labels config, to detect a per-series cache
    // built under a different (e.g. other shard's, mid-reconfiguration) config.
    size_t _labels_to_aggregate_by_hash;
    label_key scratch_key;
    map_type _values;

    void add(const seastar::metrics::impl::metric_value& m, label_key_view key, const labels_type& input_labels) noexcept {
        auto i = _values.find(key);
        if (i == _values.end()) {
            _values.emplace(label_key{std::string(key.key), key.hash}, labels_value{build_aggregated_labels(input_labels), m});
        } else {
            i->second.m += m;
        }
    }

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
        add(m, label_key_view{scratch_key.key, scratch_key.hash}, input_labels);
    }

    /*!
     * \brief add a metric using a precomputed aggregation key
     *
     * Same as add() above, but uses the aggregation key already cached on value_info (see
     * metrics::impl::metric_series_metadata) instead of rebuilding it from the full label
     * set on every call. The cached key is looked up by reference; it is only copied into
     * an owned map key on a miss, i.e. once per output group rather than once per series.
     * This is the path used by the prometheus scrape handlers; the label-based add() above
     * is kept for direct testing of this class and any other caller.
     */
    void add(const seastar::metrics::impl::metric_value& m, const seastar::metrics::impl::metric_series_metadata& value_info) noexcept {
        if (value_info.has_aggregation_cache() && value_info.aggregation_key_config_hash() == _labels_to_aggregate_by_hash) [[likely]] {
            add(m, label_key_view{value_info.aggregation_key(), value_info.aggregation_key_hash()}, value_info.labels());
        } else {
            // Rare: either this shard's metadata hasn't picked up an aggregate_labels
            // change yet (no cache), or it rebuilt under a different config than the
            // one driving this scrape (stale cache, cross-shard reconfiguration race).
            // Recompute instead of trusting a cache that isn't there or doesn't match.
            add(m, value_info.labels());
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
