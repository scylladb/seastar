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

#include <functional>
#include "sstring.hh"
#include "core/shared_ptr.hh"
#include "core/metrics_registration.hh"

/*! \file metrics.hh
 *  \brief header for metrics creation.
 *
 *  This header file contains the metrics creation method with their helper function.
 *  Include this file when need to create metrics.
 *  Typically this will be in your source file.
 *
 *  Code that is under the impl namespace should not be used directly.
 *
 */

namespace seastar {

/*!
 * \namespace metrics
 * \brief metrics creation and registration
 *
 * the metrics namespace holds the relevant method and classes to generate metrics.
 *
 * The metrics layer support registering metrics, that later will be
 * exported via different API protocols.
 *
 * To be able to support multiple protocols the following simplifications where made:
 * 1. The id of the metrics is based on the collectd id
 * 2. A metric could be a single value either a reference or a function
 *
 * To add metrics definition to class A do the following:
 * * Add a metrics_group memeber to A
 * * Add a a set_metrics() method that would be called in the constructor.
 *
 *
 * In A header file
 * \code
 * #include "core/metrics_registration.hh"
 * class A {
 *   metric_groups _metrics
 *
 *   void setup_metrics();
 *
 * };
 * \endcode
 *
 * In A source file:
 *
 * \code
 * include "core/metrics.hh"
 *
 * void A::setup_metrics() {
 *   namespace sm = seastar::metrics;
 *   _metrics = sm::create_metric_group();
 *   _metrics->add_group("cache", {sm::make_gauge("bytes", "used", [this] { return _region.occupancy().used_space(); })});
 * }
 * \endcode
 */

namespace metrics {


/*!
 * \defgroup metrics_types metrics type definitions
 * The following are for the metric layer use, do not use them directly
 * Instead use the make_counter, make_gauge, make_absolute and make_derived
 *
 */
using group_name_type = sstring; /*!< A group of logically related metrics */
using measurement_type = sstring; /*!<  What is being measured like: objects, latency, etc'*/
using sub_measurement_type = sstring; /*!<  Sub measurement like (memory bytes) used, free, etc'*/
using instance_id_type = sstring; /*!<  typically used for the shard id*/

/*!
 * \brief Human-readable description of a metric/group.
 *
 *
 * Uses a separate class to deal with type resolution
 *
 * Add this to metric creation:
 *
 * \code
 * _metrics->add_group("groupname", {
 *   sm::make_gauge("measurement", "subm-masurement", value, description("A documentation about the return value"))
 * });
 * \endcode
 *
 */
class description {
public:
    description(sstring s = sstring()) : _s(std::move(s))
    {}
    const sstring& str() const {
        return _s;
    }
private:
    sstring _s;
};

/*!
 * \namesapce impl
 * \brief holds the implementation parts of the metrics layer, do not use directly.
 *
 * The metrics layer define a thin API for adding metrics.
 * Some of the implementation details need to be in the header file, they should not be use directly.
 */
namespace impl {

// The value binding data types
enum class data_type : uint8_t {
    COUNTER, // unsigned int 64
    GAUGE, // double
    DERIVE, // signed int 64
    ABSOLUTE, // unsigned int 64
};

/*!
 * \breif A helper class that used to return metrics value.
 *
 * Do not use directly @see metrics_creation
 */
struct metric_value {
    union {
        double _d;
        uint64_t _ui;
        int64_t _i;
    } u;
    data_type _type;

    data_type type() const {
        return _type;
    }

    double d() const {
        return u._d;
    }

    uint64_t ui() const {
        return u._ui;
    }

    int64_t i() const {
        return u._i;
    }

    metric_value()
            : _type(data_type::GAUGE) {
    }

    template<typename T>
    metric_value(T i, data_type t)
            : _type(t) {
        switch (_type) {
        case data_type::DERIVE:
            u._i = i;
            break;
        case data_type::GAUGE:
            u._d = i;
            break;
        default:
            u._ui = i;
            break;
        }
    }

    metric_value& operator=(const metric_value& c) = default;

    metric_value& operator+=(const metric_value& c) {
        *this = *this + c;
        return *this;
    }

    metric_value operator+(const metric_value& c);
};

using metric_function = std::function<metric_value()>;

struct metric_definition {
    measurement_type mt;
    sub_measurement_type smt;
    instance_id_type id;
    data_type dt;
    metric_function f;
    description d;
    bool enabled = true;
};

class metric_groups_def {
public:
    metric_groups_def() = default;
    virtual ~metric_groups_def() = default;
    metric_groups_def(const metric_groups_def&) = delete;
    metric_groups_def(metric_groups_def&&) = default;
    virtual metric_groups_def& add_metric(group_name_type name, const metric_definition& md) = 0;
    virtual metric_groups_def& add_group(group_name_type name, const std::initializer_list<metric_definition>& l) = 0;
    virtual metric_groups_def& add_group(group_name_type name, const std::vector<metric_definition>& l) = 0;
};

instance_id_type shard();

extern const bool metric_disabled;


template<typename T, typename En = std::true_type>
struct is_callable;

template<typename T>
struct is_callable<T, typename std::integral_constant<bool, !std::is_void<typename std::result_of<T()>::type>::value>::type> : public std::true_type {
};

template<typename T>
struct is_callable<T, typename std::enable_if<std::is_fundamental<T>::value, std::true_type>::type> : public std::false_type {
};

template<typename T, typename = std::enable_if_t<is_callable<T>::value>>
metric_function make_function(T val, data_type dt) {
    return [dt, val] {
        return metric_value(val(), dt);
    };
}

template<typename T, typename = std::enable_if_t<!is_callable<T>::value>>
metric_function make_function(T& val, data_type dt) {
    return [dt, &val] {
        return metric_value(val, dt);
    };
}
}
/*
 * The metrics definition are defined to be compatible with collectd metrics defintion.
 * Typically you should used gauge or derived.
 */


/*!
 * \brief Gauge are a general purpose metric.
 *
 * They can support floating point and can increase or decrease
 */
template<typename T>
impl::metric_definition make_gauge(measurement_type measurement,
        sub_measurement_type sm, T val, description d=description(), bool enabled=true,
        instance_id_type instance = impl::shard()) {
    return {measurement, sm, instance, impl::data_type::GAUGE, make_function(val, impl::data_type::GAUGE), d, enabled};
}

/*!
 * \brief Derive are used when a rate is more interesting than the value.
 *
 * Derive is an integer value that can increase or decrease, typically it is used when looking at the
 * derivation of the value.
 *
 * It is OK to use it when counting things and if no wrap-around is expected (it shouldn't) it's prefer over counter metric.
 */
template<typename T>
impl::metric_definition make_derive(measurement_type measurement,
        sub_measurement_type sm, T val, description d=description(), bool enabled=true,
        instance_id_type instance = impl::shard()) {
    return {measurement, sm, instance, impl::data_type::DERIVE, make_function(val, impl::data_type::DERIVE), d, enabled};
}

/*!
 * \brief create a counter metric
 *
 * Counters are similar to derived, but they assume monotony, so if a counter value decrease in a series it is count as a wrap-around.
 * It is better to use large enough data value than to use counter.
 *
 */
template<typename T>
impl::metric_definition make_counter(measurement_type measurement,
        sub_measurement_type sm, T val, description d=description(), bool enabled=true,
        instance_id_type instance = impl::shard()) {
    return {measurement, sm, instance, impl::data_type::COUNTER, make_function(val, impl::data_type::COUNTER), d, enabled};
}

/*!
 * \brief create an absolute metric.
 *
 * Absolute are used for metric that are being erased after each time they are read.
 * They are here for compatibility reasons and should general be avoided in most applications.
 */
template<typename T>
impl::metric_definition make_absolute(measurement_type measurement,
        sub_measurement_type sm, T val, description d=description(), bool enabled=true,
        instance_id_type instance = impl::shard()) {
    return {measurement, sm, instance, impl::data_type::ABSOLUTE, make_function(val, impl::data_type::ABSOLUTE), d, enabled};
}

/*! @} */
}
}
