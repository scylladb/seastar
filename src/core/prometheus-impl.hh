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

#include <unordered_map>
#include <vector>
#include <string>

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
    std::vector<std::string> _labels_to_aggregate_by;
    std::unordered_map<labels_type, seastar::metrics::impl::metric_value> _values;
public:
    metric_aggregate_by_labels(std::vector<std::string> labels) : _labels_to_aggregate_by(std::move(labels)) {
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
    void add(const seastar::metrics::impl::metric_value& m, labels_type labels) noexcept {
        for (auto&& l : _labels_to_aggregate_by) {
            labels.erase(l);
        }
        std::unordered_map<labels_type, seastar::metrics::impl::metric_value>::iterator i = _values.find(labels);
        if ( i == _values.end()) {
            _values.emplace(std::move(labels), m);
        } else {
            i->second += m;
        }
    }
    const std::unordered_map<labels_type, seastar::metrics::impl::metric_value>& get_values() const noexcept {
        return _values;
    }
    bool empty() const noexcept {
        return _values.empty();
    }
};


}
