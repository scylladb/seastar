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
 * Copyright (C) 2016 ScyllaDB
 */

#include "prometheus.hh"
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
#include "proto/metrics2.pb.h"
#include <sstream>

#include "scollectd_api.hh"
#include "scollectd-impl.hh"
#include "metrics_api.hh"
#include "http/function_handlers.hh"
#include <boost/algorithm/string/replace.hpp>
#include <boost/range/algorithm_ext/erase.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/range/combine.hpp>

namespace seastar {

namespace prometheus {
namespace pm = io::prometheus::client;

namespace mi = metrics::impl;

/**
 * Taken from an answer in stackoverflow:
 * http://stackoverflow.com/questions/2340730/are-there-c-equivalents-for-the-protocol-buffers-delimited-i-o-functions-in-ja
 */
static bool write_delimited_to(const google::protobuf::MessageLite& message,
        google::protobuf::io::ZeroCopyOutputStream* rawOutput) {
    google::protobuf::io::CodedOutputStream output(rawOutput);

    const int size = message.ByteSize();
    output.WriteVarint32(size);

    uint8_t* buffer = output.GetDirectBufferForNBytesAndAdvance(size);
    if (buffer != nullptr) {
        message.SerializeWithCachedSizesToArray(buffer);
    } else {
        message.SerializeWithCachedSizes(&output);
        if (output.HadError()) {
            return false;
        }
    }

    return true;
}

static pm::Metric* add_label(pm::Metric* mt, const metrics::impl::metric_id & id, const config& ctx) {
    mt->mutable_label()->Reserve(id.labels().size() + 1);
    auto label = mt->add_label();
    label->set_name("instance");
    label->set_value(ctx.hostname);
    for (auto &&i : id.labels()) {
        label = mt->add_label();
        label->set_name(i.first);
        label->set_value(i.second);
    }
    return mt;
}

static void fill_metric(pm::MetricFamily& mf, const metrics::impl::metric_value& c,
        const metrics::impl::metric_id & id, const config& ctx) {
    switch (c.type()) {
    case scollectd::data_type::DERIVE:
        add_label(mf.add_metric(), id, ctx)->mutable_counter()->set_value(c.i());
        mf.set_type(pm::MetricType::COUNTER);
        break;
    case scollectd::data_type::GAUGE:
        add_label(mf.add_metric(), id, ctx)->mutable_gauge()->set_value(c.d());
        mf.set_type(pm::MetricType::GAUGE);
        break;
    case scollectd::data_type::HISTOGRAM:
    {
        auto h = c.get_histogram();
        auto mh = add_label(mf.add_metric(), id,ctx)->mutable_histogram();
        mh->set_sample_count(h.sample_count);
        mh->set_sample_sum(h.sample_sum);
        for (auto b : h.buckets) {
            auto bc = mh->add_bucket();
            bc->set_cumulative_count(b.count);
            bc->set_upper_bound(b.upper_bound);
        }
        mf.set_type(pm::MetricType::HISTOGRAM);
        break;
    }
    default:
        add_label(mf.add_metric(), id, ctx)->mutable_counter()->set_value(c.ui());
        mf.set_type(pm::MetricType::COUNTER);
        break;
    }
}

using metrics_families_per_shard = std::vector<foreign_ptr<mi::values_reference>>;

static future<> get_map_value(metrics_families_per_shard& vec) {
    vec.resize(smp::count);
    return parallel_for_each(boost::irange(0u, smp::count), [&vec] (auto cpu) {
        return smp::submit_to(cpu, [] {
            return mi::get_values();
        }).then([&vec, cpu] (auto res) {
            vec[cpu] = std::move(res);
        });
    });
}

static std::string to_str(seastar::metrics::impl::data_type dt) {
    switch (dt) {
    case seastar::metrics::impl::data_type::GAUGE:
        return "gauge";
    case seastar::metrics::impl::data_type::COUNTER:
        return "counter";
    case seastar::metrics::impl::data_type::HISTOGRAM:
        return "histogram";
    case seastar::metrics::impl::data_type::DERIVE:
        // Prometheus server does not respect derive parameters
        // So we report them as counter
        return "counter";
    default:
        break;
    }
    return "untyped";
}

static std::string to_str(const seastar::metrics::impl::metric_value& v) {
    switch (v.type()) {
    case seastar::metrics::impl::data_type::GAUGE:
        return std::to_string(v.d());
    case seastar::metrics::impl::data_type::COUNTER:
        return std::to_string(v.i());
    case seastar::metrics::impl::data_type::DERIVE:
        return std::to_string(v.ui());
    default:
        break;
    }
    return ""; // we should never get here but it makes the compiler happy
}

static void add_name(std::ostream& s, const sstring& name, const std::map<sstring, sstring>& labels, const config& ctx) {
    s << name << "{instance=\"" << ctx.hostname << '"';
    if (!labels.empty()) {
        for (auto l : labels) {
            s << "," << l.first  << "=\"" << l.second << '"';
        }
    }
    s << "} ";

}

/*!
 * \brief iterator for metric family
 *
 * In prometheus, a single shard collecct all the data from the other
 * shards and report it.
 *
 * Each shard returns a value_copy struct that has a vector of vector values (a vector per metric family)
 * and a vector of metadata (and insdie it a vector of metric metadata)
 *
 * The metrics are sorted by the metric family name.
 *
 * In prometheus, all the metrics that belongs to the same metric family are reported together.
 *
 * For efficiency the results from the metrics layer are kept in a vector.
 *
 * So we have a vector of shards of a vector of metric families of a vector of values.
 *
 * To produce the result, we use the metric_family_iterator that is created by metric_family_range.
 *
 * When iterating over the metrics we use two helper structure.
 *
 * 1. A map between metric family name and the total number of values (combine on all shards) and
 *    pointer to the metric family metadata.
 * 2. A vector of positions to the current metric family for each shard.
 *
 * The metric_family_range returns a metric_family_iterator that goes over all the families.
 *
 * The iterator returns a metric_family object, that can report the metric_family name, the size (how many
 * metrics in total belongs to the metric family) and a a foreach_metric method.
 *
 * The foreach_metric method can be used to perform an action on each of the metric that belongs to
 * that metric family
 *
 * Iterating over the metrics is done:
 * - go over each of the shard and each of the entry in the position vector:
 *   - if the current family (the metric family that we get from the shard and position) has the current name:
 *     - iterate over each of the metrics belong to that metric family:
 *
 * for example, if m is a metric_family_range
 *
 * for (auto&& i : m) {
 *   std::cout << i.name() << std::endl;
 *   i.foreach_metric([](const mi::metric_value& value, const mi::metric_info& value_info) {
 *     std::cout << value_info.id.labels().size() <<std::cout;
 *   });
 * }
 *
 * Will print all the metric family names followed by the number of labels each metric has.
 */
class metric_family_iterator;

/*!
 * \brief a facade class for metric family
 */
class metric_family {
    const sstring* _name = nullptr;
    uint32_t _size = 0;
    const mi::metric_family_info* _family_info = nullptr;
    metric_family_iterator& _iterator_state;
    metric_family(metric_family_iterator& state) : _iterator_state(state) {
    }
    metric_family(const sstring* name , uint32_t size, const mi::metric_family_info* family_info, metric_family_iterator& state) :
        _name(name), _size(size), _family_info(family_info), _iterator_state(state) {
    }
    metric_family(const metric_family& info, metric_family_iterator& state) :
        metric_family(info._name, info._size, info._family_info, state) {
    }
public:
    metric_family(const metric_family&) = delete;
    metric_family(metric_family&&) = delete;

    const sstring& name() const {
        return *_name;
    }

    const uint32_t size() const {
        return _size;
    }

    const mi::metric_family_info& metadata() const {
        return *_family_info;
    }

    void foreach_metric(std::function<void(const mi::metric_value&, const mi::metric_info&)>&& f);

    bool end() const {
        return !_name || !_family_info;
    }
    friend class metric_family_iterator;
};

class metric_family_iterator {
    const metrics_families_per_shard& _families;
    std::vector<size_t> _positions;
    metric_family _info;

    void next() {
        if (_positions.empty()) {
            return;
        }
        const sstring *new_name = nullptr;
        const mi::metric_family_info* new_family_info = nullptr;
        _info._size = 0;
        for (auto&& i : boost::combine(_positions, _families)) {
            auto& pos_in_metric_per_shard = boost::get<0>(i);
            auto& metric_family = boost::get<1>(i);
            if (pos_in_metric_per_shard >= metric_family->metadata->size()) {
                // no more metric family in this shard
                continue;
            }
            auto& metadata = metric_family->metadata->at(pos_in_metric_per_shard);
            int cmp = (!new_name) ? -1 : metadata.mf.name.compare(*new_name);
            if (cmp < 0) {
                new_name = &metadata.mf.name;
                new_family_info = &metadata.mf;
                _info._size = 0;
            }
            if (cmp <= 0) {
                _info._size += metadata.metrics.size();
            }
        }
        _info._name = new_name;
        _info._family_info = new_family_info;
    }

public:
    metric_family_iterator() = delete;
    metric_family_iterator(const metric_family_iterator& o) : _families(o._families), _positions(o._positions), _info(o._info, *this) {
        next();
    }

    metric_family_iterator(metric_family_iterator&& o) : _families(o._families), _positions(std::move(o._positions)),
            _info(o._info, *this) {
        next();
    }

    metric_family_iterator(const metrics_families_per_shard& families,
            unsigned shards)
        : _families(families), _positions(shards, 0), _info(*this) {
        next();
    }

    metric_family_iterator& operator++() {
        next();
        return *this;
    }

    metric_family_iterator operator++(int) {
        metric_family_iterator previous(*this);
        next();
        return previous;
    }

    bool operator!=(const metric_family_iterator& o) const {
        return !(*this == o);
    }

    bool operator==(const metric_family_iterator& o) const {
        if (end()) {
            return o.end();
        }
        if (o.end()) {
            return false;
        }
        return name() == o.name();
    }

    metric_family& operator*() {
        return _info;
    }

    metric_family* operator->() {
        return &_info;
    }
    const sstring& name() const {
        return *_info._name;
    }

    const uint32_t size() const {
        return _info._size;
    }

    const mi::metric_family_info& metadata() const {
        return *_info._family_info;
    }

    bool end() const {
        return _positions.empty() || _info.end();
    }

    void foreach_metric(std::function<void(const mi::metric_value&, const mi::metric_info&)>&& f) {
        // iterating over the shard vector and the position vector
        for (auto&& i : boost::combine(_positions, _families)) {
            auto& pos_in_metric_per_shard = boost::get<0>(i);
            auto& metric_family = boost::get<1>(i);
            if (pos_in_metric_per_shard >= metric_family->metadata->size()) {
                // no more metric family in this shard
                continue;
            }
            auto& metadata = metric_family->metadata->at(pos_in_metric_per_shard);
            // the the name is different, that means that on this shard, the metric family
            // does not exist, because everything is sorted by metric family name, this is fine.
            if (metadata.mf.name == name()) {
                const mi::value_vector& values = metric_family->values[pos_in_metric_per_shard];
                const mi::metric_metadata_vector& metrics_metadata = metadata.metrics;
                for (auto&& vm : boost::combine(values, metrics_metadata)) {
                    auto& value = boost::get<0>(vm);
                    auto& metric_metadata = boost::get<1>(vm);
                    f(value, metric_metadata);
                }
                pos_in_metric_per_shard++;
            }
        }
    }

};

void metric_family::foreach_metric(std::function<void(const mi::metric_value&, const mi::metric_info&)>&& f) {
    _iterator_state.foreach_metric(std::move(f));
}

class metric_family_range {
    const metrics_families_per_shard& _families;
public:
    metric_family_range(const metrics_families_per_shard& families) : _families(families) {
    }

    metric_family_iterator begin () const {
        return metric_family_iterator(_families, smp::count);
    }


    metric_family_iterator end() const {
        return metric_family_iterator(_families, 0);
    }
};

future<> write_text_representation(output_stream<char>& out, const metrics_families_per_shard& families, const config& ctx) {
    return do_with(metric_family_range(families), false,
            [&ctx, &out](auto& m, auto& found) mutable {
        return do_for_each(m, [&out, &found, &ctx] (metric_family& metric_family) mutable {
            std::stringstream s;
            auto name = ctx.prefix + "_" + metric_family.name();
            found = false;
            metric_family.foreach_metric([&s, &ctx, &found, &name, &metric_family](auto value, auto value_info) mutable {
                if (!found) {
                    if (metric_family.metadata().d.str() != "") {
                        s << "# HELP " << name << " " <<  metric_family.metadata().d.str() << "\n";
                    }
                    s << "# TYPE " << name << " " << to_str(metric_family.metadata().type) << "\n";
                    found = true;
                }
                if (value.type() == mi::data_type::HISTOGRAM) {
                    auto&& h = value.get_histogram();
                    std::map<sstring, sstring> labels = value_info.id.labels();
                    auto& le = labels["le"];
                    uint64_t count = 0;
                    auto bucket = name + "_bucket";
                    for (auto  i : h.buckets) {
                         le = std::to_string(i.upper_bound);
                        count += i.count;
                        add_name(s, bucket, labels, ctx);
                        s << count;
                        s << "\n";
                    }
                    labels["le"] = "+Inf";
                    add_name(s, bucket, labels, ctx);
                    s << h.sample_count;
                    s << "\n";

                    add_name(s, name + "_sum", {}, ctx);
                    s << h.sample_sum;
                    s << "\n";
                    add_name(s, name + "_count", {}, ctx);
                    s << h.sample_count;
                    s << "\n";

                } else {
                    add_name(s, name, value_info.id.labels(), ctx);
                    s << to_str(value);
                    s << "\n";
                }
            });
            return out.write(s.str());
        });
    });
}

future<> write_protobuf_representation(output_stream<char>& out, const metrics_families_per_shard& families, const config& ctx) {
    return do_with(metric_family_range(families),
            [&ctx, &out](auto& m) mutable {
        return do_for_each(m, [&ctx, &out](metric_family& metric_family) mutable {
            std::string s;
            google::protobuf::io::StringOutputStream os(&s);

            auto& name = metric_family.name();
            pm::MetricFamily mtf;

            mtf.set_name(ctx.prefix + "_" + name);
            mtf.mutable_metric()->Reserve(metric_family.size());
            metric_family.foreach_metric([&mtf, &ctx](auto value, auto value_info) {
                fill_metric(mtf, value, value_info.id, ctx);
            });
            if (!write_delimited_to(mtf, &os)) {
                seastar_logger.warn("Failed to write protobuf metrics");
            }
            return out.write(s);
        });
    });
}

bool is_accept_text(const std::string& accept) {
    std::vector<std::string> strs;
    boost::split(strs, accept, boost::is_any_of(","));
    for (auto i : strs) {
        boost::trim(i);
        if (boost::starts_with(i, "application/vnd.google.protobuf;")) {
            return false;
        }
    }
    return true;
}

class metrics_handler : public handler_base  {
    sstring _prefix;
    config _ctx;

public:
    metrics_handler(config ctx) : _ctx(ctx) {}

    future<std::unique_ptr<httpd::reply>> handle(const sstring& path,
        std::unique_ptr<httpd::request> req, std::unique_ptr<httpd::reply> rep) override {
        auto text = is_accept_text(req->get_header("Accept"));
        rep->write_body((text) ? "txt" : "proto", [this, text] (output_stream<char>&& s) {
            return do_with(metrics_families_per_shard(), output_stream<char>(std::move(s)),
                    [this, text] (metrics_families_per_shard& families, output_stream<char>& s) mutable {
                return get_map_value(families).then([&s, &families, this, text]() mutable {
                    return (text) ? write_text_representation(s, families, _ctx) :
                            write_protobuf_representation(s, families, _ctx);
                }).finally([&s] () mutable {
                    return s.close();
                });
            });
        });
        return make_ready_future<std::unique_ptr<httpd::reply>>(std::move(rep));
    }
};



future<> add_prometheus_routes(http_server& server, config ctx) {
    if (ctx.hostname == "") {
        ctx.hostname = metrics::impl::get_local_impl()->get_config().hostname;
    }
    server._routes.put(GET, "/metrics", new metrics_handler(ctx));
    return make_ready_future<>();
}

future<> add_prometheus_routes(distributed<http_server>& server, config ctx) {
    return server.invoke_on_all([ctx](http_server& s) {
        return add_prometheus_routes(s, ctx);
    });
}

future<> start(httpd::http_server_control& http_server, config ctx) {
    return add_prometheus_routes(http_server.server(), ctx);
}

}
}
