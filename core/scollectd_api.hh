/*
 * Copyright 2015 Cloudius Systems
 */

#ifndef CORE_SCOLLECTD_API_HH_
#define CORE_SCOLLECTD_API_HH_

#include "core/scollectd.hh"
#include "core/metrics_api.hh"

namespace scollectd {

using collectd_value  = seastar::metrics::impl::metric_value;

std::vector<collectd_value> get_collectd_value(
        const scollectd::type_instance_id& id);

std::vector<scollectd::type_instance_id> get_collectd_ids();

sstring get_collectd_description_str(const scollectd::type_instance_id&);

bool is_enabled(const scollectd::type_instance_id& id);
/**
 * Enable or disable collectd metrics on local instance
 * @id - the metric to enable or disable
 * @enable - should the collectd metrics be enable or disable
 */
void enable(const scollectd::type_instance_id& id, bool enable);


seastar::metrics::impl::value_map get_value_map();
}

#endif /* CORE_SCOLLECTD_API_HH_ */
