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

using namespace seastar;

namespace prometheus {
namespace pm = io::prometheus::client;

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

using metrics_families = seastar::metrics::impl::values_copy;

static future<metrics_families> get_map_value(std::vector<metrics::impl::values_copy>& vec, const sstring& prefix) {
    vec.resize(smp::count);
    return parallel_for_each(boost::irange(0u, smp::count), [&vec] (auto cpu) {
        return smp::submit_to(cpu, [] {
            return metrics::impl::get_values();
        }).then([&vec, cpu] (auto res) {
            vec[cpu] = res;
        });
    }).then([&vec, prefix]() mutable {
        metrics_families families;
        for (auto&& shard : vec) {
            for (auto&& metric : shard) {
                families[metric.first].insert(families[metric.first].end(), metric.second.begin(), metric.second.end());
            }
        }
        return families;
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

std::string get_text_representation(const metrics_families& families, const config& ctx) {
    std::stringstream s;
    for (auto name_metrics : families) {
        auto&& name = ctx.prefix + "_" + name_metrics.first;
        auto&& metrics = name_metrics.second;
        if (metrics.size() == 0) {
            continue;
        }
        const seastar::metrics::impl::registered_metric& reg_metrics = *std::get<seastar::metrics::impl::register_ref>(metrics[0]);

        if (reg_metrics.get_description().str() != "") {
            s << "# HELP " << name << " " <<  reg_metrics.get_description().str() << "\n";
        }
        s << "# TYPE " << name << " " << to_str(reg_metrics.get_type()) << "\n";
        for (auto&& pmetric : metrics) {
            const seastar::metrics::impl::registered_metric& reg = *std::get<seastar::metrics::impl::register_ref>(pmetric) ;
            auto&& id = reg.get_id();
            auto&& value = std::get<seastar::metrics::impl::metric_value>(pmetric);

            if (value.type() == seastar::metrics::impl::data_type::HISTOGRAM) {
                auto&& h = value.get_histogram();
                std::map<sstring, sstring> labels = id.labels();
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
                add_name(s, name, id.labels(), ctx);
                s << to_str(value);
                s << "\n";
            }
        }

    }
    return s.str();
}

std::string get_protobuf_representation(const metrics_families& families, const config& ctx) {
    std::string s;
    google::protobuf::io::StringOutputStream os(&s);
    for (auto name_metrics : families) {
        auto&& name = name_metrics.first;
        auto&& metrics = name_metrics.second;
        pm::MetricFamily mtf;
        mtf.set_name(ctx.prefix + "_" + name);
        for (auto pmetric : metrics) {
            const seastar::metrics::impl::registered_metric& reg = *std::get<seastar::metrics::impl::register_ref>(pmetric);
            auto&& id = reg.get_id();
            auto&& value = std::get<seastar::metrics::impl::metric_value>(pmetric);
            fill_metric(mtf, value, id, ctx);
        }
        if (!write_delimited_to(mtf, &os)) {
            seastar_logger.warn("Failed to write protobuf metrics");
        }
    }
    return s;
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
        return do_with(std::vector<metrics::impl::values_copy>(), [rep = std::move(rep), this, req=std::move(req)] (auto& vec) mutable {
            return get_map_value(vec, _ctx.prefix).then([rep = std::move(rep), this, req=std::move(req)] (metrics_families families) mutable {
                auto text = is_accept_text(req->get_header("Accept"));
                std::string s = (text) ? get_text_representation(families, _ctx) :
                        get_protobuf_representation(families, _ctx);
                rep->_content = std::move(s);
                rep->set_content_type((text) ? "txt" : "proto");
                return make_ready_future<std::unique_ptr<reply>>(std::move(rep));
            });
        });
    }
};


future<> start(httpd::http_server_control& http_server, config ctx) {
    if (ctx.hostname == "") {
        ctx.hostname = metrics::impl::get_local_impl()->get_config().hostname;
    }

    return http_server.set_routes([ctx](httpd::routes& r) {
        r.put(GET, "/metrics", new metrics_handler(ctx));
    });
}

}
