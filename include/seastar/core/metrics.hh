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
#include <limits>
#include <seastar/core/sstring.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/core/metrics_registration.hh>
#include <boost/lexical_cast.hpp>
#include <map>
#include <seastar/core/metrics_types.hh>
#include <seastar/util/std-compat.hh>

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
 * \addtogroup metrics
 * @{
 *
 * \namespace seastar::metrics
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

class double_registration : public std::runtime_error {
public:
    double_registration(std::string what);
};

/*!
 * \defgroup metrics_types metrics type definitions
 * The following are for the metric layer use, do not use them directly
 * Instead use the make_counter, make_gauge, make_absolute and make_derived
 *
 */
using metric_type_def = sstring; /*!< Used to hold an inherit type (like bytes)*/
using metric_name_type = sstring; /*!<  The metric name'*/
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
 *   sm::make_gauge("metric_name", value, description("A documentation about the return value"))
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
 * \brief Label a metrics
 *
 * Label are useful for adding information about a metric that
 * later you would need to aggregate by.
 * For example, if you have multiple queues on a shard.
 * Adding the queue id as a Label will allow you to use the same name
 * of the metrics with multiple id instances.
 *
 * label_instance holds an instance of label consist of a key and value.
 *
 * Typically you will not generate a label_instance yourself, but use a label
 * object for that.
 * @see label for more information
 *
 *
 */
class label_instance {
    sstring _key;
    sstring _value;
public:
    /*!
     * \brief create a label_instance
     * label instance consists of key and value.
     * The key is an sstring.
     * T - the value type can be any type that can be lexical_cast to string
     * (ie. if it support the redirection operator for stringstream).
     *
     * All primitive types are supported so all the following examples are valid:
     * label_instance a("smp_queue", 1)
     * label_instance a("my_key", "my_value")
     * label_instance a("internal_id", -1)
     */
    template<typename T>
    label_instance(const sstring& key, T v) : _key(key), _value(boost::lexical_cast<std::string>(v)){}

    /*!
     * \brief returns the label key
     */
    const sstring key() const {
        return _key;
    }

    /*!
     * \brief returns the label value
     */
    const sstring value() const {
        return _value;
    }
    bool operator<(const label_instance&) const;
    bool operator==(const label_instance&) const;
    bool operator!=(const label_instance&) const;
};


/*!
 * \brief Class that creates label instances
 *
 * A factory class to create label instance
 * Typically, the same Label name is used in multiple places.
 * label is a label factory, you create it once, and use it to create the label_instance.
 *
 * In the example we would like to label the smp_queue with with the queue owner
 *
 * seastar::metrics::label smp_owner("smp_owner");
 *
 * now, when creating a new smp metric we can add a label to it:
 *
 * sm::make_queue_length("send_batch_queue_length", _last_snt_batch, {smp_owner(cpuid)})
 *
 * where cpuid in this case is unsiged.
 */
class label {
    sstring key;
public:
    using instance = label_instance;
    /*!
     * \brief creating a label
     * key is the label name, it will be the key for all label_instance
     * that will be created from this label.
     */
    explicit label(const sstring& key) : key(key) {
    }

    /*!
     * \brief creating a label instance
     *
     * Use the function operator to create a new label instance.
     * T - the value type can be any type that can be lexical_cast to string
     * (ie. if it support the redirection operator for stringstream).
     *
     * All primitive types are supported so if lab is a label, all the following examples are valid:
     * lab(1)
     * lab("my_value")
     * lab(-1)
     */
    template<typename T>
    instance operator()(T value) const {
        return label_instance(key, std::forward<T>(value));
    }

    /*!
     * \brief returns the label name
     */
    const sstring& name() const {
        return key;
    }
};

/*!
 * \namespace impl
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
    HISTOGRAM,
};

/*!
 * \brief A helper class that used to return metrics value.
 *
 * Do not use directly @see metrics_creation
 */
class metric_value {
public:
    std::variant<double, histogram> u;
    data_type _type;
    data_type type() const {
        return _type;
    }

    double d() const {
        return std::get<double>(u);
    }

    uint64_t ui() const {
        auto d = std::get<double>(u);
        if (d >= 0 && d <= double(std::numeric_limits<long>::max())) {
            return lround(d);
        } else {
            // double value is out of range or NaN or Inf
            ulong_conversion_error(d);
            return 0;
        }
    }

    int64_t i() const {
        auto d = std::get<double>(u);
        if (d >= double(std::numeric_limits<long>::min()) && d <= double(std::numeric_limits<long>::max())) {
            return lround(d);
        } else {
            // double value is out of range or NaN or Inf
            ulong_conversion_error(d);
            return 0;
        }
    }

    metric_value()
            : _type(data_type::GAUGE) {
    }

    metric_value(histogram&& h, data_type t = data_type::HISTOGRAM) :
        u(std::move(h)), _type(t) {
    }
    metric_value(const histogram& h, data_type t = data_type::HISTOGRAM) :
        u(h), _type(t) {
    }

    metric_value(double d, data_type t)
            : u(d), _type(t) {
    }

    metric_value& operator=(const metric_value& c) = default;

    metric_value& operator+=(const metric_value& c) {
        *this = *this + c;
        return *this;
    }

    metric_value operator+(const metric_value& c);
    const histogram& get_histogram() const {
        return std::get<histogram>(u);
    }

private:
    static void ulong_conversion_error(double d);
};

using metric_function = std::function<metric_value()>;

struct metric_type {
    data_type base_type;
    metric_type_def type_name;
};

struct metric_definition_impl {
    metric_name_type name;
    metric_type type;
    metric_function f;
    description d;
    bool enabled = true;
    std::map<sstring, sstring> labels;
    metric_definition_impl& operator ()(bool enabled);
    metric_definition_impl& operator ()(const label_instance& label);
    metric_definition_impl& set_type(const sstring& type_name);
    metric_definition_impl(
        metric_name_type name,
        metric_type type,
        metric_function f,
        description d,
        std::vector<label_instance> labels);
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

template<typename T, typename En = std::true_type>
struct is_callable;

template<typename T>
struct is_callable<T, typename std::integral_constant<bool, !std::is_void<std::invoke_result_t<T>>::value>::type> : public std::true_type {
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

extern const bool metric_disabled;

extern label shard_label;

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
impl::metric_definition_impl make_gauge(metric_name_type name,
        T&& val, description d=description(), std::vector<label_instance> labels = {}) {
    return {name, {impl::data_type::GAUGE, "gauge"}, make_function(std::forward<T>(val), impl::data_type::GAUGE), d, labels};
}

/*!
 * \brief Gauge are a general purpose metric.
 *
 * They can support floating point and can increase or decrease
 */
template<typename T>
impl::metric_definition_impl make_gauge(metric_name_type name,
        description d, T&& val) {
    return {name, {impl::data_type::GAUGE, "gauge"}, make_function(std::forward<T>(val), impl::data_type::GAUGE), d, {}};
}

/*!
 * \brief Gauge are a general purpose metric.
 *
 * They can support floating point and can increase or decrease
 */
template<typename T>
impl::metric_definition_impl make_gauge(metric_name_type name,
        description d, std::vector<label_instance> labels, T&& val) {
    return {name, {impl::data_type::GAUGE, "gauge"}, make_function(std::forward<T>(val), impl::data_type::GAUGE), d, labels};
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
impl::metric_definition_impl make_derive(metric_name_type name,
        T&& val, description d=description(), std::vector<label_instance> labels = {}) {
    return {name, {impl::data_type::DERIVE, "derive"}, make_function(std::forward<T>(val), impl::data_type::DERIVE), d, labels};
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
impl::metric_definition_impl make_derive(metric_name_type name, description d,
        T&& val) {
    return {name, {impl::data_type::DERIVE, "derive"}, make_function(std::forward<T>(val), impl::data_type::DERIVE), d, {}};
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
impl::metric_definition_impl make_derive(metric_name_type name, description d, std::vector<label_instance> labels,
        T&& val) {
    return {name, {impl::data_type::DERIVE, "derive"}, make_function(std::forward<T>(val), impl::data_type::DERIVE), d, labels};
}


/*!
 * \brief create a counter metric
 *
 * Counters are similar to derived, but they assume monotony, so if a counter value decrease in a series it is count as a wrap-around.
 * It is better to use large enough data value than to use counter.
 *
 */
template<typename T>
impl::metric_definition_impl make_counter(metric_name_type name,
        T&& val, description d=description(), std::vector<label_instance> labels = {}) {
    return {name, {impl::data_type::COUNTER, "counter"}, make_function(std::forward<T>(val), impl::data_type::COUNTER), d, labels};
}

/*!
 * \brief create an absolute metric.
 *
 * Absolute are used for metric that are being erased after each time they are read.
 * They are here for compatibility reasons and should general be avoided in most applications.
 */
template<typename T>
impl::metric_definition_impl make_absolute(metric_name_type name,
        T&& val, description d=description(), std::vector<label_instance> labels = {}) {
    return {name, {impl::data_type::ABSOLUTE, "absolute"}, make_function(std::forward<T>(val), impl::data_type::ABSOLUTE), d, labels};
}

/*!
 * \brief create a histogram metric.
 *
 * Histograms are a list o buckets with upper values and counter for the number
 * of entries in each bucket.
 */
template<typename T>
impl::metric_definition_impl make_histogram(metric_name_type name,
        T&& val, description d=description(), std::vector<label_instance> labels = {}) {
    return  {name, {impl::data_type::HISTOGRAM, "histogram"}, make_function(std::forward<T>(val), impl::data_type::HISTOGRAM), d, labels};
}

/*!
 * \brief create a histogram metric.
 *
 * Histograms are a list o buckets with upper values and counter for the number
 * of entries in each bucket.
 */
template<typename T>
impl::metric_definition_impl make_histogram(metric_name_type name,
        description d, std::vector<label_instance> labels, T&& val) {
    return  {name, {impl::data_type::HISTOGRAM, "histogram"}, make_function(std::forward<T>(val), impl::data_type::HISTOGRAM), d, labels};
}


/*!
 * \brief create a histogram metric.
 *
 * Histograms are a list o buckets with upper values and counter for the number
 * of entries in each bucket.
 */
template<typename T>
impl::metric_definition_impl make_histogram(metric_name_type name,
        description d, T&& val) {
    return  {name, {impl::data_type::HISTOGRAM, "histogram"}, make_function(std::forward<T>(val), impl::data_type::HISTOGRAM), d, {}};
}


/*!
 * \brief create a total_bytes metric.
 *
 * total_bytes are used for an ever growing counters, like the total bytes
 * passed on a network.
 */

template<typename T>
impl::metric_definition_impl make_total_bytes(metric_name_type name,
        T&& val, description d=description(), std::vector<label_instance> labels = {},
        instance_id_type instance = impl::shard()) {
    return make_derive(name, std::forward<T>(val), d, labels).set_type("total_bytes");
}

/*!
 * \brief create a current_bytes metric.
 *
 * current_bytes are used to report on current status in bytes.
 * For example the current free memory.
 */

template<typename T>
impl::metric_definition_impl make_current_bytes(metric_name_type name,
        T&& val, description d=description(), std::vector<label_instance> labels = {},
        instance_id_type instance = impl::shard()) {
    return make_gauge(name, std::forward<T>(val), d, labels).set_type("bytes");
}


/*!
 * \brief create a queue_length metric.
 *
 * queue_length are used to report on queue length
 */

template<typename T>
impl::metric_definition_impl make_queue_length(metric_name_type name,
        T&& val, description d=description(), std::vector<label_instance> labels = {},
        instance_id_type instance = impl::shard()) {
    return make_gauge(name, std::forward<T>(val), d, labels).set_type("queue_length");
}


/*!
 * \brief create a total operation metric.
 *
 * total_operations are used for ever growing operation counter.
 */

template<typename T>
impl::metric_definition_impl make_total_operations(metric_name_type name,
        T&& val, description d=description(), std::vector<label_instance> labels = {},
        instance_id_type instance = impl::shard()) {
    return make_derive(name, std::forward<T>(val), d, labels).set_type("total_operations");
}

/*! @} */
}
}
