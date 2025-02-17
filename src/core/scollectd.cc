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

#ifdef SEASTAR_MODULE
module;
#endif

#include <sys/socket.h>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <utility>
#include <string>
#include <map>
#include <iostream>
#include <unordered_map>

#ifdef SEASTAR_MODULE
module seastar;
#else
#include <seastar/core/seastar.hh>
#include <seastar/core/scollectd_api.hh>
#include <seastar/core/metrics_api.hh>
#include <seastar/core/byteorder.hh>
#include <seastar/core/print.hh>
#include <seastar/core/shared_ptr.hh>

#include "core/scollectd-impl.hh"
#endif
#include <seastar/util/assert.hh>

namespace seastar {

void scollectd::type_instance_id::truncate(sstring& field, const char* field_desc) {
    if (field.size() > max_collectd_field_text_len) {
        auto suffix_len = std::ceil(std::log10(++_next_truncated_idx)) + 1;
        sstring new_field(seastar::format(
            "{}~{:d}", sstring(field.data(), max_collectd_field_text_len - suffix_len), _next_truncated_idx));

        logger.warn("Truncating \"{}\" to {} chars: \"{}\" -> \"{}\"", field_desc, max_collectd_field_text_len, field,
            new_field);
        field = std::move(new_field);
    }
}

bool scollectd::type_instance_id::operator<(
        const scollectd::type_instance_id& id2) const {
    auto& id1 = *this;
    return std::tie(id1.plugin(), id1.plugin_instance(), id1.type(),
            id1.type_instance())
            < std::tie(id2.plugin(), id2.plugin_instance(), id2.type(),
                    id2.type_instance());
}
bool scollectd::type_instance_id::operator==(
        const scollectd::type_instance_id & id2) const {
    auto& id1 = *this;
    return std::tie(id1.plugin(), id1.plugin_instance(), id1.type(),
            id1.type_instance())
            == std::tie(id2.plugin(), id2.plugin_instance(), id2.type(),
                    id2.type_instance());
}

namespace scollectd {

::seastar::logger logger("scollectd");
thread_local unsigned type_instance_id::_next_truncated_idx = 0;

registration::~registration() {
    unregister();
}

registration::registration(const type_instance_id& id)
: _id(id), _impl(seastar::metrics::impl::get_local_impl()) {
}

registration::registration(type_instance_id&& id)
: _id(std::move(id)), _impl(seastar::metrics::impl::get_local_impl()) {
}

seastar::metrics::impl::metric_id to_metrics_id(const type_instance_id & id) {
    seastar::metrics::impl::labels_type labels {{seastar::metrics::shard_label.name(), seastar::metrics::impl::shard()}};
    auto internalized_labels = make_lw_shared<seastar::metrics::impl::labels_type>(std::move(labels));
    return seastar::metrics::impl::metric_id(id.plugin(), id.type_instance(), std::move(internalized_labels));
}


const plugin_instance_id per_cpu_plugin_instance("#cpu");

static const size_t payload_size = 1024;

enum class part_type : uint16_t {
    Host = 0x0000, // The name of the host to associate with subsequent data values
    Time = 0x0001, // Time  Numeric The timestamp to associate with subsequent data values, unix time format (seconds since epoch)
    TimeHr = 0x0008, // Time (high resolution)  Numeric The timestamp to associate with subsequent data values. Time is defined in 2–30 seconds since epoch. New in Version 5.0.
    Plugin = 0x0002, // Plugin String The plugin name to associate with subsequent data values, e.g. "cpu"
    PluginInst = 0x0003, // Plugin instance String  The plugin instance name to associate with subsequent data values, e.g. "1"
    Type = 0x0004, // Type String The type name to associate with subsequent data values, e.g. "cpu"
    TypeInst = 0x0005, // Type instance     String  The type instance name to associate with subsequent data values, e.g. "idle"
    Values = 0x0006, // Values  other   Data values, see above
    Interval = 0x0007, // Interval Numeric Interval used to set the "step" when creating new RRDs unless rrdtool plugin forces StepSize. Also used to detect values that have timed out.
    IntervalHr = 0x0009, // Interval (high resolution)  Numeric     The interval in which subsequent data values are collected. The interval is given in 2–30 seconds. New in Version 5.0.
    Message = 0x0100, // Message (notifications) String
    Severity = 0x0101, // Severity  Numeric
    Signature = 0x0200, // Signature (HMAC-SHA-256)     other (todo)
    Encryption = 0x0210, // Encryption (AES-256/OFB
};

// "Time is defined in 2^–30 seconds since epoch. New in Version 5.0."
typedef std::chrono::duration<uint64_t, std::ratio<1, 0x40000000>> collectd_hres_duration;

// yet another writer type, this one to construct collectd network
// protocol data.
struct cpwriter {
    typedef std::array<char, payload_size> buffer_type;
    typedef buffer_type::iterator mark_type;
    typedef buffer_type::const_iterator const_mark_type;

    buffer_type _buf = {};
    mark_type _pos;
    bool _overflow = false;

    std::unordered_map<uint16_t, sstring> _cache;

    cpwriter()
            : _pos(_buf.begin()) {
    }
    mark_type mark() const {
        return _pos;
    }
    bool overflow() const {
        return _overflow;
    }
    void reset(mark_type m) {
        _pos = m;
        _overflow = false;
    }
    size_t size() const {
        return std::distance(_buf.begin(), const_mark_type(_pos));
    }
    bool empty() const {
        return _pos == _buf.begin();
    }
    void clear() {
        reset(_buf.begin());
        _cache.clear();
        _overflow = false;
    }
    const char * data() const {
        return &_buf.at(0);
    }
    char * data() {
        return &_buf.at(0);
    }
    cpwriter& check(size_t sz) {
        size_t av = std::distance(_pos, _buf.end());
        _overflow |= av < sz;
        return *this;
    }

    sstring get_type_instance(const metrics::metric_name_type& name, const metrics::impl::labels_type& labels) {
        if (labels.empty()) {
            return name;
        }
        sstring res = name;
        for (auto i : labels) {
            if (i.first != seastar::metrics::shard_label.name()) {
                res += "-" + i.second;
            }
        }
        return res;
    }
    explicit operator bool() const {
        return !_overflow;
    }
    bool operator!() const {
        return !operator bool();
    }
    template<typename _Iter>
    cpwriter & write(_Iter s, _Iter e) {
        if (check(std::distance(s, e))) {
            _pos = std::copy(s, e, _pos);
        }
        return *this;
    }
    template<typename T>
    std::enable_if_t<std::is_integral_v<T>, cpwriter &> write(
            const T & t) {
        T tmp = net::hton(t);
        auto * p = reinterpret_cast<const uint8_t *>(&tmp);
        auto * e = p + sizeof(T);
        write(p, e);
        return *this;
    }
    template<typename T>
    std::enable_if_t<std::is_integral_v<T>, cpwriter &> write_le(const T & t) {
        T tmp = cpu_to_le(t);
        auto * p = reinterpret_cast<const uint8_t *>(&tmp);
        auto * e = p + sizeof(T);
        write(p, e);
        return *this;
    }
    void write_value(const seastar::metrics::impl::metric_value& v) {
        switch (v.type()) {
            case data_type::GAUGE: {
                double tmpd = v.d();
                uint64_t tmpi;
                std::copy_n(reinterpret_cast<const char*>(&tmpd), 8, reinterpret_cast<char*>(&tmpi));
                write_le(tmpi);
                break;
            }
            case data_type::COUNTER:
            case data_type::REAL_COUNTER:
                write(v.ui()); // unsigned int 64, big endian
                break;
            default:
                SEASTAR_ASSERT(0);
        }
    }
    cpwriter & write(const sstring & s) {
        write(s.begin(), s.end() + 1); // include \0
        return *this;
    }
    cpwriter & put(part_type type, const sstring & s) {
        write(uint16_t(type));
        write(uint16_t(4 + s.size() + 1)); // include \0
        write(s); // include \0
        return *this;
    }
    cpwriter & put_cached(part_type type, const sstring & s) {
        auto & cached = _cache[uint16_t(type)];
        if (cached != s) {
            put(type, s);
            cached = s;
        }
        return *this;
    }
    template<typename T>
    std::enable_if_t<std::is_integral_v<T>, cpwriter &> put(
            part_type type, T t) {
        write(uint16_t(type));
        write(uint16_t(4 + sizeof(t)));
        write(t);
        return *this;
    }
    cpwriter & put(part_type type, const value_list & v) {
        auto s = v.size();
        auto sz = 6 + s + s * sizeof(uint64_t);
        if (check(sz)) {
            write(uint16_t(type));
            write(uint16_t(sz));
            write(uint16_t(s));
            v.types(reinterpret_cast<data_type *>(&(*_pos)));
            _pos += s;
            v.values(reinterpret_cast<net::packed<uint64_t> *>(&(*_pos)));
            _pos += s * sizeof(uint64_t);
        }
        return *this;
    }

    cpwriter & put(part_type type, const seastar::metrics::impl::metric_value & v) {
        auto sz = 7 +  sizeof(uint64_t);
        if (check(sz)) {
            write(uint16_t(type));
            write(uint16_t(sz));
            write(uint16_t(1));
            write(static_cast<uint8_t>(v.type()));
            write_value(v);
        }
        return *this;
    }
    cpwriter & put(const sstring & host, const metrics::group_name_type& group_name, const metrics::metric_name_type& name,
            const metrics::impl::labels_type& labels, const type_id& type) {
        const auto ts = std::chrono::system_clock::now().time_since_epoch();
        const auto lrts =
                std::chrono::duration_cast<std::chrono::seconds>(ts).count();

        put_cached(part_type::Host, host);
        put(part_type::Time, uint64_t(lrts));
        // Seems hi-res timestamp does not work very well with
        // at the very least my default collectd in fedora (or I did it wrong?)
        // Use lo-res ts for now, it is probably quite sufficient.
        put_cached(part_type::Plugin, group_name);
        // Optional
        auto instance_id = labels.at(metrics::shard_label.name());
        put_cached(part_type::PluginInst,
                instance_id == per_cpu_plugin_instance ?
                        to_sstring(this_shard_id()) : instance_id);
        put_cached(part_type::Type, type);
        // Optional
        put_cached(part_type::TypeInst, get_type_instance(name, labels));
        return *this;
    }
    cpwriter & put(const sstring & host,
            const duration & period,
            const type_instance_id & id, const value_list & v) {
        const auto ps = std::chrono::duration_cast<collectd_hres_duration>(
                        period).count();
            auto mid = to_metrics_id(id);
            put(host, mid.group_name(), mid.name(), mid.labels(), id.type());
            put(part_type::Values, v);
            if (ps != 0) {
                put(part_type::IntervalHr, ps);
            }
            return *this;
    }

    cpwriter & put(const sstring & host,
            const duration & period,
            const type_id& type,
            const metrics::group_name_type& group_name, const metrics::metric_name_type& name,
            const metrics::impl::labels_type& labels, const seastar::metrics::impl::metric_value & v) {
        const auto ps = std::chrono::duration_cast<collectd_hres_duration>(
                period).count();
        put(host, group_name, name, labels, type);
        put(part_type::Values, v);
        if (ps != 0) {
            put(part_type::IntervalHr, ps);
        }
        return *this;
    }
};

void impl::add_polled(const type_instance_id & id,
        const shared_ptr<value_list> & values, bool enable) {
    // do nothing
    // add_polled is now implemented on the metrics layer

}

void impl::remove_polled(const type_instance_id & id) {
    seastar::metrics::impl::unregister_metric(to_metrics_id(id));
}

// explicitly send a type_instance value list (outside polling)
future<> impl::send_metric(const type_instance_id & id,
        const value_list & values) {
    if (values.empty()) {
        return make_ready_future();
    }
    cpwriter out;
    out.put(_host, duration(), id, values);
    return _chan.send(_addr, net::packet(out.data(), out.size()));
}

future<> impl::send_notification(const type_instance_id & id,
        const sstring & msg) {
    cpwriter out;
    auto mid = to_metrics_id(id);
    out.put(_host, mid.group_name(), mid.name(), mid.labels(), id.type());
    out.put(part_type::Message, msg);
    return _chan.send(_addr, net::packet(out.data(), out.size()));
}

// initiates actual value polling -> send to target "loop"
void impl::start(const sstring & host, const ipv4_addr & addr, const duration period) {
    _period = period;
    _addr = addr;
    _host = host;
    _chan = make_unbound_datagram_channel(AF_INET);
    _timer.set_callback(std::bind(&impl::run, this));

    // dogfood ourselves
    namespace sm = seastar::metrics;

    _metrics.add_group("scollectd", {
        // total_bytes      value:DERIVE:0:U
        sm::make_counter("total_bytes_sent", sm::description("total bytes sent"), _bytes),
        // total_requests      value:DERIVE:0:U
        sm::make_counter("total_requests", sm::description("total requests"), _num_packets),
        // latency          value:GAUGE:0:U
        sm::make_gauge("latency", sm::description("avrage latency"), _avg),
        // total_time_in_ms    value:DERIVE:0:U
        sm::make_counter("total_time_in_ms", sm::description("total time in milliseconds"), _millis),
        // total_values     value:DERIVE:0:U
        sm::make_gauge("total_values", sm::description("current number of values reported"), [this] {return values().size();}),
        // records          value:GAUGE:0:U
        sm::make_gauge("records", sm::description("number of records reported"), [this] {return values().size();}),
    });

    // FIXME: future is discarded
    (void)send_notification(
            type_instance_id("scollectd", per_cpu_plugin_instance,
                    "network"), "daemon started");
    arm();
}

void impl::stop() {
    _timer.cancel();
    _metrics.clear();
}


void impl::arm() {
    if (_period != duration()) {
        _timer.arm(_period);
    }
}

void impl::run() {
    typedef size_t metric_family_id;
    typedef seastar::metrics::impl::value_vector::iterator value_iterator;
    typedef seastar::metrics::impl::metric_metadata_fifo::iterator metadata_iterator;
    typedef std::tuple<metric_family_id, metadata_iterator, value_iterator, type_id, cpwriter> context;

    auto ctxt = make_lw_shared<context>();
    foreign_ptr<shared_ptr<seastar::metrics::impl::values_copy>> vals =  seastar::metrics::impl::get_values();

    // note we're doing this unsynced since we assume
    // all registrations to this instance will be done on the
    // same cpu, and without interuptions (no wait-states)

    auto& values = vals->values;
    auto metadata = vals->metadata;
    std::get<metric_family_id>(*ctxt) = 0;
    if (values.size() > 0) {
        std::get<value_iterator>(*ctxt) = values[0].begin();
        std::get<metadata_iterator>(*ctxt) = metadata->at(0).metrics.begin();
        std::get<type_id>(*ctxt) = metadata->at(0).mf.inherit_type;
    }

    auto stop_when = [ctxt, metadata]() {
        auto done = std::get<metric_family_id>(*ctxt) == metadata->size();
        return done;
    };
    // append as many values as we can fit into a packet (1024 bytes)
    auto send_packet = [this, ctxt, &values, metadata]() mutable {
        auto start = steady_clock_type::now();
        auto& mf = std::get<metric_family_id>(*ctxt);
        auto & md_iterator = std::get<metadata_iterator>(*ctxt);
        auto & i = std::get<value_iterator>(*ctxt);
        auto & out = std::get<cpwriter>(*ctxt);

        out.clear();

        bool out_of_space = false;
        while (!out_of_space && mf < values.size()) {
            while (i != values[mf].end()) {
                if (i->type() == seastar::metrics::impl::data_type::HISTOGRAM) {
                    ++i;
                    ++md_iterator;
                    continue;
                }
                auto m = out.mark();
                out.put(_host, _period, std::get<type_id>(*ctxt),
                    md_iterator->group_name(), md_iterator->name(), md_iterator->labels(), *i);
                if (!out) {
                    out.reset(m);
                    out_of_space = true;
                    break;
                }
                ++i;
                ++md_iterator;
            }
            if (out_of_space) {
                break;
            }
            ++mf;
            if (mf < values.size()) {
                i = values[mf].begin();
                md_iterator = metadata->at(mf).metrics.begin();
                std::get<type_id>(*ctxt) = metadata->at(mf).mf.inherit_type;
            }
        }
        if (out.empty()) {
            return make_ready_future();
        }
        return _chan.send(_addr, net::packet(out.data(), out.size())).then([start, ctxt, this]() {
                    auto & out = std::get<cpwriter>(*ctxt);
                    auto now = steady_clock_type::now();
                    // dogfood stats
                    ++_num_packets;
                    _millis += std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
                    _bytes += out.size();
                    _avg = double(_millis) / _num_packets;
                }).then_wrapped([] (auto&& f) {
                    try {
                        f.get();
                    } catch (std::exception & ex) {
                        std::cout << "send failed: " << ex.what() << std::endl;
                    } catch (...) {
                        std::cout << "send failed: - unknown exception" << std::endl;
                    }
                });
    };
    // No need to wait for future.
    // The caller has to call impl::stop() to synchronize.
    (void)do_until(stop_when, send_packet).finally([this, vals = std::move(vals)]() mutable {
        arm();
    });
}

std::vector<type_instance_id> impl::get_instance_ids() const {
    std::vector<type_instance_id> res;
    for (auto&& v: values()) {
        // Need to check for empty value_list, since unreg is two-stage.
        // Not an issue for most uses, but unit testing etc that would like
        // fully deterministic operation here would like us to only return
        // actually active ids
        for (auto i : v.second) {
            if (i.second) {
                res.emplace_back(i.second->get_id(), v.second.info().inherit_type);
            }
        }
    }
    return res;
}

void add_polled(const type_instance_id & id,
        const shared_ptr<value_list> & values, bool enabled) {
    get_impl().add_polled(id, values, enabled);
}

void remove_polled_metric(const type_instance_id & id) {
    get_impl().remove_polled(id);
}

future<> send_notification(const type_instance_id & id,
        const sstring & msg) {
    return get_impl().send_notification(id, msg);
}

future<> send_metric(const type_instance_id & id,
        const value_list & values) {
    return get_impl().send_metric(id, values);
}

void configure(const options& opts) {
    bool enable = opts.collectd.get_value();
    if (!enable) {
        return;
    }
    auto addr = ipv4_addr(opts.collectd_address.get_value());
    auto period = std::chrono::milliseconds(opts.collectd_poll_period.get_value());

    auto host = (opts.collectd_hostname.get_value() == "")
            ? seastar::metrics::impl::get_local_impl()->get_config().hostname
            : sstring(opts.collectd_hostname.get_value());

    // Now create send loops on each cpu
    for (unsigned c = 0; c < smp::count; c++) {
        // FIXME: future is discarded
        (void)smp::submit_to(c, [=] () {
            get_impl().start(host, addr, period);
        });
    }
}

options::options(program_options::option_group* parent_group)
    : program_options::option_group(parent_group, "COLLECTD options")
    , collectd(*this, "collectd", false,
            "enable collectd daemon")
    , collectd_address(*this, "collectd-address",
            "239.192.74.66:25826",
            "address to send/broadcast metrics to")
    , collectd_poll_period(*this, "collectd-poll-period",
            1000,
            "poll period - frequency of sending counter metrics (default: 1000ms, 0 disables)")
    , collectd_hostname(*this, "collectd-hostname",
            "",
            "Deprecated option, use metrics-hostname instead")
{
}

static seastar::metrics::impl::register_ref get_register(const scollectd::type_instance_id& i) {
    seastar::metrics::impl::metric_id id = to_metrics_id(i);
    return seastar::metrics::impl::get_value_map().at(id.full_name()).at(id.internalized_labels());
}

std::vector<collectd_value> get_collectd_value(
        const scollectd::type_instance_id& id) {
    std::vector<collectd_value> vals;
    const seastar::metrics::impl::registered_metric& val = *get_register(id);
    vals.push_back(val());
    return vals;
}

std::vector<scollectd::type_instance_id> get_collectd_ids() {
    return get_impl().get_instance_ids();
}

bool is_enabled(const scollectd::type_instance_id& id) {
    return get_register(id)->is_enabled();
}

void enable(const scollectd::type_instance_id& id, bool enable) {
    get_register(id)->set_enabled(enable);
}

type_instance_id plugin_instance_metrics::add_impl(const typed_value& v) {
    type_instance_id id(_plugin_id, _plugin_instance, v.type(), v.type_instance());
    get_impl().add_polled(id, v.values());
    return id;
}

void plugin_instance_metrics::add(const typed_value& v) {
    _registrations.emplace_back(add_impl(v));
}

std::vector<type_instance_id> plugin_instance_metrics::bound_ids() const {
    std::vector<type_instance_id> res;
    res.reserve(_registrations.size());
    std::transform(_registrations.begin(), _registrations.end(), std::back_inserter(res), [](const registration& r) {
       return r._id;
    });
    return res;
}

type_id type_id_for(known_type t) {
    switch (t) {
    case known_type::absolute:
        return "absolute";
    case known_type::backends:
        return "backends";
    case known_type::bitrate:
        return "bitrate";
    case known_type::blocked_clients:
        return "blocked_clients";
    case known_type::bytes:
        return "bytes";
    case known_type::cache_eviction:
        return "cache_eviction";
    case known_type::cache_operation:
        return "cache_operation";
    case known_type::cache_ratio:
        return "cache_ratio";
    case known_type::cache_result:
        return "cache_result";
    case known_type::cache_size:
        return "cache_size";
    case known_type::capacity:
        return "capacity";
    case known_type::changes_since_last_save:
        return "changes_since_last_save";
    case known_type::charge:
        return "charge";
    case known_type::clock_last_meas:
        return "clock_last_meas";
    case known_type::clock_last_update:
        return "clock_last_update";
    case known_type::clock_mode:
        return "clock_mode";
    case known_type::clock_reachability:
        return "clock_reachability";
    case known_type::clock_skew_ppm:
        return "clock_skew_ppm";
    case known_type::clock_state:
        return "clock_state";
    case known_type::clock_stratum:
        return "clock_stratum";
    case known_type::compression:
        return "compression";
    case known_type::compression_ratio:
        return "compression_ratio";
    case known_type::connections:
        return "connections";
    case known_type::conntrack:
        return "conntrack";
    case known_type::contextswitch:
        return "contextswitch";
    case known_type::count:
        return "count";
    case known_type::counter:
        return "counter";
    case known_type::cpu:
        return "cpu";
    case known_type::cpufreq:
        return "cpufreq";
    case known_type::current:
        return "current";
    case known_type::current_connections:
        return "current_connections";
    case known_type::current_sessions:
        return "current_sessions";
    case known_type::delay:
        return "delay";
    case known_type::derive:
        return "derive";
    case known_type::df:
        return "df";
    case known_type::df_complex:
        return "df_complex";
    case known_type::df_inodes:
        return "df_inodes";
    case known_type::disk_io_time:
        return "disk_io_time";
    case known_type::disk_latency:
        return "disk_latency";
    case known_type::disk_merged:
        return "disk_merged";
    case known_type::disk_octets:
        return "disk_octets";
    case known_type::disk_ops:
        return "disk_ops";
    case known_type::disk_ops_complex:
        return "disk_ops_complex";
    case known_type::disk_time:
        return "disk_time";
    case known_type::dns_answer:
        return "dns_answer";
    case known_type::dns_notify:
        return "dns_notify";
    case known_type::dns_octets:
        return "dns_octets";
    case known_type::dns_opcode:
        return "dns_opcode";
    case known_type::dns_qtype:
        return "dns_qtype";
    case known_type::dns_qtype_cached:
        return "dns_qtype_cached";
    case known_type::dns_query:
        return "dns_query";
    case known_type::dns_question:
        return "dns_question";
    case known_type::dns_rcode:
        return "dns_rcode";
    case known_type::dns_reject:
        return "dns_reject";
    case known_type::dns_request:
        return "dns_request";
    case known_type::dns_resolver:
        return "dns_resolver";
    case known_type::dns_response:
        return "dns_response";
    case known_type::dns_transfer:
        return "dns_transfer";
    case known_type::dns_update:
        return "dns_update";
    case known_type::dns_zops:
        return "dns_zops";
    case known_type::drbd_resource:
        return "drbd_resource";
    case known_type::duration:
        return "duration";
    case known_type::email_check:
        return "email_check";
    case known_type::email_count:
        return "email_count";
    case known_type::email_size:
        return "email_size";
    case known_type::entropy:
        return "entropy";
    case known_type::evicted_keys:
        return "evicted_keys";
    case known_type::expired_keys:
        return "expired_keys";
    case known_type::fanspeed:
        return "fanspeed";
    case known_type::file_handles:
        return "file_handles";
    case known_type::file_size:
        return "file_size";
    case known_type::files:
        return "files";
    case known_type::flow:
        return "flow";
    case known_type::fork_rate:
        return "fork_rate";
    case known_type::frequency:
        return "frequency";
    case known_type::frequency_error:
        return "frequency_error";
    case known_type::frequency_offset:
        return "frequency_offset";
    case known_type::fscache_stat:
        return "fscache_stat";
    case known_type::gauge:
        return "gauge";
    case known_type::hash_collisions:
        return "hash_collisions";
    case known_type::http_request_methods:
        return "http_request_methods";
    case known_type::http_requests:
        return "http_requests";
    case known_type::http_response_codes:
        return "http_response_codes";
    case known_type::humidity:
        return "humidity";
    case known_type::if_collisions:
        return "if_collisions";
    case known_type::if_dropped:
        return "if_dropped";
    case known_type::if_errors:
        return "if_errors";
    case known_type::if_multicast:
        return "if_multicast";
    case known_type::if_octets:
        return "if_octets";
    case known_type::if_packets:
        return "if_packets";
    case known_type::if_rx_errors:
        return "if_rx_errors";
    case known_type::if_rx_octets:
        return "if_rx_octets";
    case known_type::if_tx_errors:
        return "if_tx_errors";
    case known_type::if_tx_octets:
        return "if_tx_octets";
    case known_type::invocations:
        return "invocations";
    case known_type::io_octets:
        return "io_octets";
    case known_type::io_packets:
        return "io_packets";
    case known_type::ipt_bytes:
        return "ipt_bytes";
    case known_type::ipt_packets:
        return "ipt_packets";
    case known_type::irq:
        return "irq";
    case known_type::latency:
        return "latency";
    case known_type::links:
        return "links";
    case known_type::load:
        return "load";
    case known_type::md_disks:
        return "md_disks";
    case known_type::memory:
        return "memory";
    case known_type::memory_lua:
        return "memory_lua";
    case known_type::memory_throttle_count:
        return "memory_throttle_count";
    case known_type::multimeter:
        return "multimeter";
    case known_type::mutex_operations:
        return "mutex_operations";
    case known_type::objects:
        return "objects";
    case known_type::operations:
        return "operations";
    case known_type::packets:
        return "packets";
    case known_type::pending_operations:
        return "pending_operations";
    case known_type::percent:
        return "percent";
    case known_type::percent_bytes:
        return "percent_bytes";
    case known_type::percent_inodes:
        return "percent_inodes";
    case known_type::ping:
        return "ping";
    case known_type::ping_droprate:
        return "ping_droprate";
    case known_type::ping_stddev:
        return "ping_stddev";
    case known_type::players:
        return "players";
    case known_type::power:
        return "power";
    case known_type::pressure:
        return "pressure";
    case known_type::protocol_counter:
        return "protocol_counter";
    case known_type::pubsub:
        return "pubsub";
    case known_type::queue_length:
        return "queue_length";
    case known_type::records:
        return "records";
    case known_type::requests:
        return "requests";
    case known_type::response_code:
        return "response_code";
    case known_type::response_time:
        return "response_time";
    case known_type::root_delay:
        return "root_delay";
    case known_type::root_dispersion:
        return "root_dispersion";
    case known_type::route_etx:
        return "route_etx";
    case known_type::route_metric:
        return "route_metric";
    case known_type::routes:
        return "routes";
    case known_type::segments:
        return "segments";
    case known_type::serial_octets:
        return "serial_octets";
    case known_type::signal_noise:
        return "signal_noise";
    case known_type::signal_power:
        return "signal_power";
    case known_type::signal_quality:
        return "signal_quality";
    case known_type::snr:
        return "snr";
    case known_type::spl:
        return "spl";
    case known_type::swap:
        return "swap";
    case known_type::swap_io:
        return "swap_io";
    case known_type::tcp_connections:
        return "tcp_connections";
    case known_type::temperature:
        return "temperature";
    case known_type::threads:
        return "threads";
    case known_type::time_dispersion:
        return "time_dispersion";
    case known_type::time_offset:
        return "time_offset";
    case known_type::time_offset_ntp:
        return "time_offset_ntp";
    case known_type::time_offset_rms:
        return "time_offset_rms";
    case known_type::time_ref:
        return "time_ref";
    case known_type::timeleft:
        return "timeleft";
    case known_type::total_bytes:
        return "total_bytes";
    case known_type::total_connections:
        return "total_connections";
    case known_type::total_objects:
        return "total_objects";
    case known_type::total_operations:
        return "total_operations";
    case known_type::total_requests:
        return "total_requests";
    case known_type::total_sessions:
        return "total_sessions";
    case known_type::total_threads:
        return "total_threads";
    case known_type::total_time_in_ms:
        return "total_time_in_ms";
    case known_type::total_values:
        return "total_values";
    case known_type::uptime:
        return "uptime";
    case known_type::users:
        return "users";
    case known_type::vcl:
        return "vcl";
    case known_type::vcpu:
        return "vcpu";
    case known_type::virt_cpu_total:
        return "virt_cpu_total";
    case known_type::virt_vcpu:
        return "virt_vcpu";
    case known_type::vmpage_action:
        return "vmpage_action";
    case known_type::vmpage_faults:
        return "vmpage_faults";
    case known_type::vmpage_io:
        return "vmpage_io";
    case known_type::vmpage_number:
        return "vmpage_number";
    case known_type::volatile_changes:
        return "volatile_changes";
    case known_type::voltage:
        return "voltage";
    case known_type::voltage_threshold:
        return "voltage_threshold";
    case known_type::vs_memory:
        return "vs_memory";
    case known_type::vs_processes:
        return "vs_processes";
    case known_type::vs_threads:
        return "vs_threads";
    default:
        throw std::invalid_argument("Unknown type");
    }
}

metrics::impl::value_map get_value_map() {
    return metrics::impl::get_value_map();
}

}

thread_local scollectd::impl scollectd_impl;

scollectd::impl & scollectd::get_impl() {
    return scollectd_impl;
}

}
