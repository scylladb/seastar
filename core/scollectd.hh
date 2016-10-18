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
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 */

#ifndef SCOLLECTD_HH_
#define SCOLLECTD_HH_

#include <type_traits>
#include <utility>
#include <functional>
#include <array>
#include <iterator>
#include <stdint.h>
#include <memory>
#include <string>
#include <tuple>
#include <chrono>
#include <boost/program_options.hpp>

#include "future.hh"
#include "net/byteorder.hh"
#include "core/shared_ptr.hh"
#include "core/sstring.hh"
#include "core/print.hh"
#include "util/log.hh"

#include "core/metrics_api.hh"

/**
 * Implementation of rudimentary collectd data gathering.
 *
 * Usage is hopefully straight forward. Though, feel free to read
 * https://collectd.org/wiki/index.php/Naming_schema
 * for an explanation on the naming model.
 *
 * Typically, you'll add values something like:
 *
 * 		scollectd::type_instance_id typ("<pluginname>", "<instance_name>", "<type_name>", "<instance_name>");
 *		scollectd::add_polled_metric(typ, [<metric var> | scollectd::make_typed(<data_type>, <metric_var>) [, ...]);
 *
 * Where
 * 	<pluginname> would be the overall 'module', e.g. "cpu"
 *  <instance_name> -> optional distinguisher between plugin instances. For cpu, the built-in
 *  scollectd::per_cpu_plugin_instance constant is a good choice, i.e. 0->N cpu.
 *  If there are no instances (e.g. only one), empty constant is appropriate (none)
 *  <type_name> is the 'type' of metric collected, for ex. "usage" (cpu/0/usage)
 *  <type_instance> is a distinguisher for metric parts of the type, e.g. "idle", "user", "kernel"
 *  -> cpu/0/usage/idle | cpu/0/usage/user | cpu/0/usage/kernel
 *
 *  Each type instance can bind an arbitrary number of values, ech representing some aspect in turn of the instance.
 *  The structure and interpretation is up to the producer/consumer
 *
 * There is a single "scollectd" instance per cpu, and values should be bound locally
 * to this cpu. Polling is done at a frequency set in the seastar config (def once per s),
 * and all registered values will be sent via UDP packages to the destination host(s)
 *
 * Note that the tuple { plugin, plugin_instance, type, type_instance } is considered a
 * unique ID for a value registration, so using the same tuple twice will remove the previously
 * registered values.
 *
 * Values can be unregistered at any time, though they must be so on the same thread/cpu
 * as they we're registered. The "registration" achor type provides RAII style value unregistration
 * semantics.
 *
 */

namespace scollectd {

extern seastar::logger logger;

using data_type = seastar::metrics::impl::data_type;

enum class known_type {
    // from types.db. Defined collectd types (type_id) selection.
    // This enum omits the very application specific types, such
    // as mysql_* etc, since if you really are re-writing mysql
    // in seastar, you probably know how to look the type up manually...

    absolute,
    backends,
    bitrate,
    blocked_clients,
    bytes,
    cache_eviction,
    cache_operation,
    cache_ratio,
    cache_result,
    cache_size,
    capacity,
    changes_since_last_save,
    charge,
    clock_last_meas,
    clock_last_update,
    clock_mode,
    clock_reachability,
    clock_skew_ppm,
    clock_state,
    clock_stratum,
    compression,
    compression_ratio,
    connections,
    conntrack,
    contextswitch,
    count,
    counter,
    cpu,
    cpufreq,
    current,
    current_connections,
    current_sessions,
    delay,
    derive,
    df,
    df_complex,
    df_inodes,
    disk_io_time,
    disk_latency,
    disk_merged,
    disk_octets,
    disk_ops,
    disk_ops_complex,
    disk_time,
    dns_answer,
    dns_notify,
    dns_octets,
    dns_opcode,
    dns_qtype,
    dns_qtype_cached,
    dns_query,
    dns_question,
    dns_rcode,
    dns_reject,
    dns_request,
    dns_resolver,
    dns_response,
    dns_transfer,
    dns_update,
    dns_zops,
    drbd_resource,
    duration,
    email_check,
    email_count,
    email_size,
    entropy,
    evicted_keys,
    expired_keys,
    fanspeed,
    file_handles,
    file_size,
    files,
    flow,
    fork_rate,
    frequency,
    frequency_error,
    frequency_offset,
    fscache_stat,
    gauge,
    hash_collisions,
    http_request_methods,
    http_requests,
    http_response_codes,
    humidity,
    if_collisions,
    if_dropped,
    if_errors,
    if_multicast,
    if_octets,
    if_packets,
    if_rx_errors,
    if_rx_octets,
    if_tx_errors,
    if_tx_octets,
    invocations,
    io_octets,
    io_packets,
    ipt_bytes,
    ipt_packets,
    irq,
    latency,
    links,
    load,
    md_disks,
    memory,
    memory_lua,
    memory_throttle_count,
    multimeter,
    mutex_operations,
    objects,
    operations,
    packets,
    pending_operations,
    percent,
    percent_bytes,
    percent_inodes,
    ping,
    ping_droprate,
    ping_stddev,
    players,
    power,
    pressure,
    protocol_counter,
    pubsub,
    queue_length,
    records,
    requests,
    response_code,
    response_time,
    root_delay,
    root_dispersion,
    route_etx,
    route_metric,
    routes,
    segments,
    serial_octets,
    signal_noise,
    signal_power,
    signal_quality,
    snr,
    spl,
    swap,
    swap_io,
    tcp_connections,
    temperature,
    threads,
    time_dispersion,
    time_offset,
    time_offset_ntp,
    time_offset_rms,
    time_ref,
    timeleft,
    total_bytes,
    total_connections,
    total_objects,
    total_operations,
    total_requests,
    total_sessions,
    total_threads,
    total_time_in_ms,
    total_values,
    uptime,
    users,
    vcl,
    vcpu,
    virt_cpu_total,
    virt_vcpu,
    vmpage_action,
    vmpage_faults,
    vmpage_io,
    vmpage_number,
    volatile_changes,
    voltage,
    voltage_threshold,
    vs_memory,
    vs_processes,
    vs_threads,
};

// don't use directly. use make_typed.
template<typename T>
struct typed {
    typed(data_type t, T && v)
    : type(t), value(std::forward<T>(v)) {
    }
    data_type type;
    T value;
};

template<typename T>
static inline typed<T> make_typed(data_type type, T&& t) {
    return typed<T>(type, std::forward<T>(t));
}

using plugin_id = seastar::metrics::group_name_type;
using plugin_instance_id = seastar::metrics::instance_id_type;
using type_id = seastar::metrics::measurement_type;
using type_instance = seastar::metrics::sub_measurement_type;

type_id type_id_for(known_type);

using description = seastar::metrics::description;

static constexpr unsigned max_collectd_field_text_len = 63;

class type_instance_id {
    static thread_local unsigned _next_truncated_idx;

    /// truncate a given field to the maximum allowed length
    void truncate(sstring& field, const char* field_desc) {
        if (field.size() > max_collectd_field_text_len) {
            auto suffix_len = std::ceil(std::log10(++_next_truncated_idx)) + 1;
            sstring new_field(seastar::format("{}~{:d}", sstring(field.data(), max_collectd_field_text_len - suffix_len), _next_truncated_idx));

            logger.warn("Truncating \"{}\" to {} chars: \"{}\" -> \"{}\"", field_desc, max_collectd_field_text_len, field, new_field);
            field = std::move(new_field);
        }
    }
public:
    type_instance_id() = default;
    type_instance_id(plugin_id p, plugin_instance_id pi, type_id t,
                    scollectd::type_instance ti = std::string())
                    : _plugin(std::move(p)), _plugin_instance(std::move(pi)), _type(
                                    std::move(t)), _type_instance(std::move(ti)) {
        // truncate strings to the maximum allowed length
        truncate(_plugin, "plugin");
        truncate(_plugin_instance, "plugin_instance");
        truncate(_type, "type");
        truncate(_type_instance, "type_instance");
    }
    type_instance_id(const seastar::metrics::impl::metric_id &id) : _plugin(id.group_name()),
            _plugin_instance(id.instance_id()), _type(id.measurement()),
            _type_instance(id.sub_measurement()) {
    }
    type_instance_id(type_instance_id &&) = default;
    type_instance_id(const type_instance_id &) = default;

    type_instance_id & operator=(type_instance_id &&) = default;
    type_instance_id & operator=(const type_instance_id &) = default;

    const plugin_id & plugin() const {
        return _plugin;
    }
    const plugin_instance_id & plugin_instance() const {
        return _plugin_instance;
    }
    const type_id & type() const {
        return _type;
    }
    const scollectd::type_instance & type_instance() const {
        return _type_instance;
    }
    bool operator<(const type_instance_id&) const;
    bool operator==(const type_instance_id&) const;
private:
    plugin_id _plugin;
    plugin_instance_id _plugin_instance;
    type_id _type;
    scollectd::type_instance _type_instance;
};

extern const plugin_instance_id per_cpu_plugin_instance;

void configure(const boost::program_options::variables_map&);
boost::program_options::options_description get_options_description();
void remove_polled_metric(const type_instance_id &);

class plugin_instance_metrics;

/**
 * Anchor for polled registration.
 * Iff the registered type is in some way none-persistent,
 * use this as receiver of the reg and ensure it dies before the
 * added value(s).
 *
 * Use:
 * uint64_t v = 0;
 * registration r = add_polled_metric(v);
 * ++r;
 * <scope end, above dies>
 */
struct registration {
    registration() = default;
    registration(const type_instance_id& id)
    : _id(id) {
    }
    registration(type_instance_id&& id)
    : _id(std::move(id)) {
    }
    registration(const registration&) = delete;
    registration(registration&&) = default;
    ~registration();
    registration & operator=(const registration&) = delete;
    registration & operator=(registration&&) = default;

    void unregister() {
        remove_polled_metric(_id);
        _id = type_instance_id();
    }
private:
    friend class plugin_instance_metrics;

    type_instance_id _id;
};

/**
 * Helper type to make generating vectors of registration objects
 * easier, since it constructs from an initializer list of
 * type_instance_id:s, avoiding early conversion to registration objs,
 * which in case of init lists, are copy semantics, not move...
 */
class registrations
    : public std::vector<registration>
{
public:
    typedef std::vector<registration> vector_type;

    registrations()
    {}
    registrations(vector_type&& v) : vector_type(std::move(v))
    {}
    registrations(const std::initializer_list<type_instance_id>& l)
        : vector_type(l.begin(),l.end())
    {}
    registrations& operator=(vector_type&& v) {
        vector_type::operator=(std::move(v));
        return *this;
    }
    registrations& operator=(const std::initializer_list<type_instance_id>& l) {
        return registrations::operator=(registrations(l));
    }
};

class value_list;

struct typed_value {
    /**
     * Wraps N values of a given type (type_id).
     * Used to group types into a plugin_instance_metrics
     */
    template<typename... Args>
    typed_value(const type_id& tid, const scollectd::type_instance& ti, description, Args&&... args);

    template<typename... Args>
    typed_value(const type_id& tid, const scollectd::type_instance& ti, Args&&... args)
        : typed_value(tid, ti, description(), std::forward<Args>(args)...)
    {}

    const scollectd::type_instance& type_instance() const {
        return _type_instance;
    }
    const shared_ptr<value_list>& values() const {
        return _values;
    }
    const type_id & type() const {
        return _type_id;
    }
private:
    type_id _type_id;
    scollectd::type_instance _type_instance;
    ::shared_ptr<value_list> _values;
};

class plugin_instance_metrics {
public:
    template<typename... TypedValues>
    plugin_instance_metrics(const plugin_id& p, const plugin_instance_id& pi, TypedValues&&... values)
        : _plugin_id(p)
        , _plugin_instance(pi)
        , _registrations({ add_impl(values)... })
    {}
    std::vector<type_instance_id> bound_ids() const;
    void add(const typed_value&);
private:
    type_instance_id add_impl(const typed_value&);

    plugin_id _plugin_id;
    plugin_instance_id _plugin_instance;
    registrations _registrations;
};

/**
 * Simplified wrapper for the common case of per-cpu plugin instances
 * (i.e. distributed objects)
 */
class percpu_plugin_instance_metrics : public plugin_instance_metrics {
public:
    template<typename... TypedValues>
    percpu_plugin_instance_metrics(const plugin_id& p, TypedValues&&... values)
        : plugin_instance_metrics(p, per_cpu_plugin_instance, std::forward<TypedValues>(values)...)
    {}
};

/**
 * Template wrapper for type_id values, deriving type_id string
 * from the known_types enum, for auto-completetion joy.
 */
template<known_type Type>
struct typed_value_impl: public typed_value {
    template<typename ... Args>
    typed_value_impl(const scollectd::type_instance& ti, Args&& ... args)
        : typed_value(type_id_for(Type), ti, std::forward<Args>(args)...)
    {}

    template<typename ... Args>
    typed_value_impl(scollectd::type_instance ti, description d, Args&& ... args)
        : typed_value(type_id_for(Type), std::move(ti), std::move(d), std::forward<Args>(args)...)
    {}
    template<typename ... Args>
    typed_value_impl(description d, Args&& ... args)
        : typed_value(type_id_for(Type), scollectd::type_instance(), std::move(d), std::forward<Args>(args)...)
    {}
};

/**
 * Some typedefs for common used types. Feel free to add.
 */
typedef typed_value_impl<known_type::total_bytes> total_bytes;
typedef typed_value_impl<known_type::total_connections> total_connections;
typedef typed_value_impl<known_type::total_objects> total_objects;
typedef typed_value_impl<known_type::total_operations> total_operations;
typedef typed_value_impl<known_type::total_requests> total_requests;
typedef typed_value_impl<known_type::total_sessions> total_sessions;
typedef typed_value_impl<known_type::total_threads> total_threads;
typedef typed_value_impl<known_type::total_time_in_ms> total_time_in_ms;
typedef typed_value_impl<known_type::total_values> total_values;
typedef typed_value_impl<known_type::queue_length> queue_length;
typedef typed_value_impl<known_type::counter> counter;
typedef typed_value_impl<known_type::count> count;
typedef typed_value_impl<known_type::gauge> gauge;

// lots of template junk to build typed value list tuples
// for registered values.
template<typename T, typename En = void>
struct data_type_for;

template<typename T, typename En = void>
struct is_callable;

template<typename T>
struct is_callable<T,
typename std::enable_if<
!std::is_void<typename std::result_of<T()>::type>::value,
void>::type> : public std::true_type {
};

template<typename T>
struct is_callable<T,
typename std::enable_if<std::is_fundamental<T>::value, void>::type> : public std::false_type {
};

template<typename T>
struct data_type_for<T,
typename std::enable_if<
std::is_integral<T>::value && std::is_unsigned<T>::value,
void>::type> : public std::integral_constant<data_type,
data_type::COUNTER> {
};
template<typename T>
struct data_type_for<T,
typename std::enable_if<
std::is_integral<T>::value && std::is_signed<T>::value, void>::type> : public std::integral_constant<
data_type, data_type::DERIVE> {
};
template<typename T>
struct data_type_for<T,
typename std::enable_if<std::is_floating_point<T>::value, void>::type> : public std::integral_constant<
data_type, data_type::GAUGE> {
};
template<typename T>
struct data_type_for<T,
typename std::enable_if<is_callable<T>::value, void>::type> : public data_type_for<
typename std::result_of<T()>::type> {
};
template<typename T>
struct data_type_for<typed<T>> : public data_type_for<T> {
};

template<typename T>
class value {
public:
    template<typename W>
    struct wrap {
        wrap(const W & v)
        : _v(v) {
        }
        const W & operator()() const {
            return _v;
        }
        const W & _v;
    };

    typedef typename std::remove_reference<T>::type value_type;
    typedef typename std::conditional<
            is_callable<typename std::remove_reference<T>::type>::value,
            value_type, wrap<value_type> >::type stored_type;

    value(const value_type & t)
    : value<T>(data_type_for<value_type>::value, t) {
    }
    value(data_type type, const value_type & t)
    : _type(type), _t(t) {
    }
    uint64_t operator()() const {
        auto v = _t();
        if (_type == data_type::GAUGE) {
            return convert(double(v));
        } else {
            uint64_t u = v;
            return convert(u);
        }
    }
    operator uint64_t() const {
        return (*this)();
    }
    operator data_type() const {
        return _type;
    }
    data_type type() const {
        return _type;
    }
private:
    // not super quick value -> protocol endian 64-bit values.
    template<typename _Iter>
    void bpack(_Iter s, _Iter e, uint64_t v) const {
        while (s != e) {
            *s++ = (v & 0xff);
            v >>= 8;
        }
    }
    template<typename V>
    typename std::enable_if<std::is_integral<V>::value, uint64_t>::type convert(
            V v) const {
        uint64_t i = v;
        // network byte order
        return ntohq(i);
    }
    template<typename V>
    typename std::enable_if<std::is_floating_point<V>::value, uint64_t>::type convert(
            V t) const {
        union {
            uint64_t i;
            double v;
        } v;
        union {
            uint64_t i;
            uint8_t b[8];
        } u;
        v.v = t;
        // intel byte order. could also obviously be faster.
        // could be ignored if we just assume we're le (for now),
        // but this is ok me thinks.
        bpack(std::begin(u.b), std::end(u.b), v.i);
        return u.i;
    }
    ;

    const data_type _type;
    const stored_type _t;
};

template<typename T>
class value<typed<T>> : public value<T> {
public:
    value(const typed<T> & args)
: value<T>(args.type, args.value) {
    }
};

class value_list {
    bool _enabled = true;
public:
    value_list(description d) : _description(std::move(d))
    {}
    value_list(value_list&&) = default;
    virtual ~value_list() {}

    virtual size_t size() const = 0;

    virtual void types(data_type *) const = 0;
    virtual void values(net::packed<uint64_t> *) const = 0;

    const description& desc() const {
        return _description;
    }

    bool empty() const {
        return size() == 0;
    }

    bool is_enabled() const {
        return _enabled;
    }

    void set_enabled(bool b) {
        _enabled = b;
    }
private:
    description _description;
};

template<typename ... Args>
class values_impl: public value_list {
public:
    static const size_t num_values = sizeof...(Args);

    values_impl(description d, Args&& ...args)
        : value_list(std::move(d))
        , _values(std::forward<Args>(args)...)
    {}

    values_impl(values_impl<Args...>&& a) = default;
    values_impl(const values_impl<Args...>& a) = default;

    size_t size() const override {
        return num_values;
    }
    void types(data_type * p) const override {
        unpack(_values, [p](Args... args) {
            std::initializer_list<data_type> tmp = { args...  };
            std::copy(tmp.begin(), tmp.end(), p);
        });
    }
    void values(net::packed<uint64_t> * p) const override {
        unpack(_values, [p](Args... args) {
            std::initializer_list<uint64_t> tmp = { args...  };
            std::copy(tmp.begin(), tmp.end(), p);
        });
    }
private:
    template<typename _Op>
    void unpack(const std::tuple<Args...>& t, _Op&& op) const {
        do_unpack(t, std::index_sequence_for<Args...> {}, std::forward<_Op>(op));
    }

    template<size_t ...S, typename _Op>
    void do_unpack(const std::tuple<Args...>& t, const std::index_sequence<S...> &, _Op&& op) const {
        op(std::get<S>(t)...);
    }

    std::tuple < Args... > _values;
};

void add_polled(const type_instance_id &, const shared_ptr<value_list> &, bool enabled = true);

typedef std::function<void()> notify_function;
template<typename... _Args>
static auto make_type_instance(description d, _Args && ... args) -> values_impl < decltype(value<_Args>(std::forward<_Args>(args)))... >
{
    return values_impl<decltype(value<_Args>(std::forward<_Args>(args)))...>(
                    std::move(d), value<_Args>(std::forward<_Args>(args))...);
}
template<typename ... _Args>
static type_instance_id add_polled_metric(const plugin_id & plugin,
        const plugin_instance_id & plugin_instance, const type_id & type,
        const scollectd::type_instance & type_instance, _Args&& ... args) {
    return add_polled_metric(plugin, plugin_instance, type, type_instance, description(),
            std::forward<_Args>(args)...);
}
template<typename ... _Args>
static type_instance_id add_polled_metric(const plugin_id & plugin,
        const plugin_instance_id & plugin_instance, const type_id & type,
        const scollectd::type_instance & type_instance, description d, _Args&& ... args) {
    return add_polled_metric(
            type_instance_id(plugin, plugin_instance, type, type_instance), std::move(d),
            std::forward<_Args>(args)...);
}
template<typename ... _Args>
static future<> send_explicit_metric(const plugin_id & plugin,
        const plugin_instance_id & plugin_instance, const type_id & type,
        const scollectd::type_instance & type_instance, _Args&& ... args) {
    return send_explicit_metric(
            type_instance_id(plugin, plugin_instance, type, type_instance),
            std::forward<_Args>(args)...);
}
template<typename ... _Args>
static notify_function create_explicit_metric(const plugin_id & plugin,
        const plugin_instance_id & plugin_instance, const type_id & type,
        const scollectd::type_instance & type_instance, _Args&& ... args) {
    return create_explicit_metric(
            type_instance_id(plugin, plugin_instance, type, type_instance),
            std::forward<_Args>(args)...);
}

seastar::metrics::impl::metric_id to_metrics_id(const type_instance_id & id);

template<typename Arg>
static type_instance_id add_polled_metric(const type_instance_id & id, description d,
        Arg&& arg, bool enabled = true) {
    namespace sm = seastar::metrics::impl;
    shared_ptr<sm::registered_metric> rm =
                ::make_shared<sm::registered_metric>(arg.type, sm::make_function(arg.value, arg.type), d, enabled);
    seastar::metrics::impl::get_local_impl().add_registration(to_metrics_id(id), rm);
    return id;
}

template<typename Arg>
static type_instance_id add_polled_metric(const type_instance_id & id,
        Arg&& arg) {
    return std::move(add_polled_metric(id, description(), std::forward<Arg>(arg)));
}


template<typename Args>
static type_instance_id add_disabled_polled_metric(const type_instance_id & id, description d,
        Args&& arg) {
    return add_polled_metric(id, d, std::forward<Args>(arg), false);
}

template<typename Args>
static type_instance_id add_disabled_polled_metric(const type_instance_id & id,
        Args&& args) {
    return add_disabled_polled_metric(id, description(), std::forward<Args>(args));
}

// "Explicit" metric sends. Sends a single value list as a message.
// Obviously not super efficient either. But maybe someone needs it sometime.
template<typename ... _Args>
static future<> send_explicit_metric(const type_instance_id & id,
        _Args&& ... args) {
    return send_metric(id, make_type_instance(std::forward<_Args>(args)...));
}
template<typename ... _Args>
static notify_function create_explicit_metric(const type_instance_id & id,
        _Args&& ... args) {
    auto list = make_type_instance(std::forward<_Args>(args)...);
    return [id, list=std::move(list)]() {
        send_metric(id, list);
    };
}

template<typename... Args>
typed_value::typed_value(const type_id& tid, const scollectd::type_instance& ti, description d, Args&&... args)
    : _type_id(tid)
    , _type_instance(ti)
    , _values(::make_shared<decltype(make_type_instance(std::move(d), std::forward<Args>(args)...))>(make_type_instance(std::move(d), std::forward<Args>(args)...)))
{}

// Send a message packet (string)
future<> send_notification(const type_instance_id & id, const sstring & msg);
};

#endif /* SCOLLECTD_HH_ */
