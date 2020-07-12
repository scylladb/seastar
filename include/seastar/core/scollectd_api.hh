/*
 * Copyright 2015 Cloudius Systems
 */

#pragma once

#include <seastar/core/scollectd.hh>
#include <seastar/core/metrics_api.hh>

namespace seastar {

namespace scollectd {

using collectd_value  = seastar::metrics::impl::metric_value;

std::vector<collectd_value> get_collectd_value(
        const scollectd::type_instance_id& id);

std::vector<scollectd::type_instance_id> get_collectd_ids();

sstring get_collectd_description_str(const scollectd::type_instance_id&);

bool is_enabled(const scollectd::type_instance_id& id);
/**
 * Enable or disable collectd metrics on local instance
 * @param id - the metric to enable or disable
 * @param enable - should the collectd metrics be enable or disable
 */
void enable(const scollectd::type_instance_id& id, bool enable);


metrics::impl::value_map get_value_map();
}

}
