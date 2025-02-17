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

#include <fmt/core.h>
#include <fmt/ostream.h>
#include <seastar/core/prometheus.hh>
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
#include "proto/metrics2.pb.h"
#include <sstream>

#include <seastar/core/metrics_api.hh>
#include <seastar/core/scollectd.hh>
#include <seastar/http/function_handlers.hh>
#include <boost/algorithm/string/replace.hpp>
#include <boost/range/algorithm_ext/erase.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/range/algorithm.hpp>
#include <boost/range/combine.hpp>
#include <seastar/core/thread.hh>
#include <seastar/core/loop.hh>
#include <seastar/util/assert.hh>
#include <ranges>
#include <regex>
#include <string_view>

namespace seastar {

extern seastar::logger seastar_logger;

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

#if GOOGLE_PROTOBUF_VERSION >= 3004000
    const size_t size = message.ByteSizeLong();
    output.WriteVarint64(size);
#else
    const int size = message.ByteSize();
    output.WriteVarint32(size);
#endif

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

static pm::Metric* add_label(pm::Metric* mt, const metrics::impl::labels_type & id, const config& ctx) {
    mt->mutable_label()->Reserve(id.size() + 1);
    if (ctx.label) {
        auto label = mt->add_label();
        label->set_name(std::string(ctx.label->key()));
        label->set_value(std::string(ctx.label->value()));
    }
    for (auto && [name, value] : id) {
        auto label = mt->add_label();
        label->set_name(std::string(name));
        label->set_value(std::string(value));
    }
    return mt;
}

static void fill_old_type_histogram(const metrics::histogram& h, ::io::prometheus::client::Histogram* mh) {
    mh->set_sample_count(h.sample_count);
    mh->set_sample_sum(h.sample_sum);
    for (auto b : h.buckets) {
        auto bc = mh->add_bucket();
        bc->set_cumulative_count(b.count);
        bc->set_upper_bound(b.upper_bound);
    }
}
/*!
 * Fill a histogram using the Prometheus native histogram representation.
 *
 * Prometheus Native histogram (also known as sparse histograms)
 * uses an exponential bucket size with a coefficient equal to 2^(2^-schema).
 * Besides the schema, a native histogram has a list of BucketSpan and a list of deltas.
 * Each entry in the list of deltas represents a nonempty bucket,
 * the bucket value is stored as a delta from the previous nonempty bucket.
 * The bucket-spans list describes the buckets ids.
 * Each back span represents multiple consecutive nonempty buckets.
 * It holds the id of the first bucket in the span of buckets and the length (number of nonempty consecutive buckets).
 */
static void fill_native_type_histogram(const metrics::histogram& h, ::io::prometheus::client::Histogram* mh) {
    mh->set_sample_count(h.sample_count);
    mh->set_sample_sum(h.sample_sum);
    size_t id = h.native_histogram.value().min_id;

    mh->set_schema(h.native_histogram.value().schema);
    double last_bucket = 0;
    double count = 0;

    size_t length = 0;
    size_t last_bucket_id = 0;
    ::io::prometheus::client::BucketSpan* bucket_span = nullptr;
    for (auto b : h.buckets) {
        // Metrics histograms are aggregated histograms
        // A non empty bucket is bigger than the previous one
        if (count < b.count) {
            // If we are not part of an existing bucket-span, create one
            if (!bucket_span) {
                bucket_span = mh->add_positive_span();
                bucket_span->set_offset(id - last_bucket_id);
                length = 0;
            }
            length++;
            mh->add_positive_delta(b.count - count - last_bucket);
            last_bucket = b.count - count;
        } else {
            // The current bucket is empty, if there is an existing bucket-span
            // set its length
            if (bucket_span) {
                bucket_span->set_length(length);
                bucket_span = nullptr;
                last_bucket_id = id;
            }
        }
        count = b.count;
        id++;
    }
    // maybe there is an open bucket span (the last bucket was part of a bucket-span)
    // set its length
    if (bucket_span) {
        bucket_span->set_length(length);
    }
}

static void fill_metric(pm::MetricFamily& mf, const metrics::impl::metric_value& c,
        const metrics::impl::labels_type & id, const config& ctx) {
    switch (c.type()) {
    case scollectd::data_type::GAUGE:
        add_label(mf.add_metric(), id, ctx)->mutable_gauge()->set_value(c.d());
        mf.set_type(pm::MetricType::GAUGE);
        break;
    case scollectd::data_type::SUMMARY: {
        auto& h = c.get_histogram();
        auto mh = add_label(mf.add_metric(), id,ctx)->mutable_summary();
        mh->set_sample_count(h.sample_count);
        mh->set_sample_sum(h.sample_sum);
        for (auto b : h.buckets) {
            auto bc = mh->add_quantile();
            bc->set_value(b.count);
            bc->set_quantile(b.upper_bound);
        }
        mf.set_type(pm::MetricType::SUMMARY);
        break;
    }
    case scollectd::data_type::HISTOGRAM:
    {
        auto& h = c.get_histogram();
        auto mh = add_label(mf.add_metric(), id,ctx)->mutable_histogram();
        mh->set_sample_count(h.sample_count);
        mh->set_sample_sum(h.sample_sum);
        if (h.native_histogram) {
            fill_native_type_histogram(h, mh);
        } else {
            fill_old_type_histogram(h, mh);
        }
        mf.set_type(pm::MetricType::HISTOGRAM);
        break;
    }
    case scollectd::data_type::REAL_COUNTER:
        [[fallthrough]];
    case scollectd::data_type::COUNTER:
        add_label(mf.add_metric(), id, ctx)->mutable_counter()->set_value(c.d());
        mf.set_type(pm::MetricType::COUNTER);
        break;
    }
}

static std::ostream& operator<<(std::ostream& os, seastar::metrics::impl::data_type dt) {
    switch (dt) {
    case seastar::metrics::impl::data_type::GAUGE:
        return os << "gauge";
    case seastar::metrics::impl::data_type::COUNTER:
    case seastar::metrics::impl::data_type::REAL_COUNTER:
        return os << "counter";
    case seastar::metrics::impl::data_type::HISTOGRAM:
        return os << "histogram";
    case seastar::metrics::impl::data_type::SUMMARY:
        return os << "summary";
    }
    return os << "untyped";
}

static std::ostream& operator<<(std::ostream& os, const seastar::metrics::impl::metric_value& v) {
    switch (v.type()) {
    case seastar::metrics::impl::data_type::GAUGE:
    case seastar::metrics::impl::data_type::REAL_COUNTER:
        fmt::print(os, "{:.6f}", v.d());
        break;
    case seastar::metrics::impl::data_type::COUNTER:
        fmt::print(os, "{}", v.i());
        break;
    case seastar::metrics::impl::data_type::HISTOGRAM:
    case seastar::metrics::impl::data_type::SUMMARY:
        break;
    }
    return os;
}

/*
 * Sanitizes the prometheus label value as per the line format rules and writes it out to the ostream:
 * > label_value can be any sequence of UTF-8 characters, but the backslash (\), double-quote ("), and
 * > line feed (\n) characters have to be escaped as \\, \", and \n, respectively.
 */
static void escape_and_write_label_value(std::ostream& s, std::string_view label_value) {
    for (char c : label_value) {
        switch (c) {
            case '\\': s << R"(\\)"; break;
            case '\"': s << R"(\")"; break;
            case '\n': s << R"(\n)"; break;
            default:   s << c;
        }
    }
}

static void add_name(std::ostream& s, const sstring& name, const std::map<sstring, sstring>& labels, const config& ctx) {
    s << name << "{";
    const char* delimiter = "";
    if (ctx.label) {
        s << ctx.label->key()  << "=\"";
        escape_and_write_label_value(s, ctx.label->value());
        s << '"';
        delimiter = ",";
    }

    if (!labels.empty()) {
        for (auto l : labels) {
            if (!boost::algorithm::starts_with(l.first, "__")) {
                s << delimiter;
                s << l.first  << "=\"";
                escape_and_write_label_value(s, l.second);
                s << '"';
                delimiter = ",";
            }
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

class metric_family_range;

class metrics_families_per_shard {
    using metrics_family_per_shard_data_container = std::vector<foreign_ptr<mi::values_reference>>;
    metrics_family_per_shard_data_container _data;
    using comp_function = std::function<bool(const sstring&, const mi::metric_family_metadata&)>;
    /*!
     * \brief find the last item in a range of metric family based on a comparator function
     *
     */
    metric_family_iterator find_bound(const sstring& family_name, comp_function comp) const;

public:

    using const_iterator = metrics_family_per_shard_data_container::const_iterator;
    using iterator = metrics_family_per_shard_data_container::iterator;
    using reference = metrics_family_per_shard_data_container::reference;
    using const_reference = metrics_family_per_shard_data_container::const_reference;

    /*!
     * \brief find the first item following a metric family range.
     * metric family are sorted, this will return the first item that is outside
     * of the range
     */
    metric_family_iterator upper_bound(const sstring& family_name) const;

    /*!
     * \brief find the first item in a range of metric family.
     * metric family are sorted, the first item, is the first to match the
     * criteria.
     */
    metric_family_iterator lower_bound(const sstring& family_name) const;

    /**
     * \defgroup Variables Global variables
     */

    /*
     * @defgroup Vector properties
     * The following methods making metrics_families_per_shard act as
     * a vector of foreign_ptr<mi::values_reference>
     * @{
     *
     *
     */
    iterator begin() {
        return _data.begin();
    }

    iterator end() {
        return _data.end();
    }

    const_iterator begin() const {
        return _data.begin();
    }

    const_iterator end() const {
        return _data.end();
    }

    void resize(size_t new_size) {
        _data.resize(new_size);
    }

    reference& operator[](size_t n) {
        return _data[n];
    }

    const_reference& operator[](size_t n) const {
        return _data[n];
    }
    /** @} */
};

static future<> get_map_value(metrics_families_per_shard& vec) {
    vec.resize(smp::count);
    return parallel_for_each(std::views::iota(0u, smp::count), [&vec] (auto cpu) {
        return smp::submit_to(cpu, [] {
            return mi::get_values();
        }).then([&vec, cpu] (auto res) {
            vec[cpu] = std::move(res);
        });
    });
}


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

    uint32_t size() const {
        return _size;
    }

    const mi::metric_family_info& metadata() const {
        return *_family_info;
    }

    void foreach_metric(std::function<void(const mi::metric_value&, const mi::metric_series_metadata&)>&& f);

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
            if (_info._name &&  pos_in_metric_per_shard < metric_family->metadata->size() &&
                    metric_family->metadata->at(pos_in_metric_per_shard).mf.name.compare(*_info._name) <= 0) {
                pos_in_metric_per_shard++;
            }
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
    metric_family_iterator(const metric_family_iterator& o) : _families(o._families), _positions(o._positions), _info(*this) {
        next();
    }

    metric_family_iterator(metric_family_iterator&& o) : _families(o._families), _positions(std::move(o._positions)),
            _info(*this) {
        next();
    }

    metric_family_iterator(const metrics_families_per_shard& families,
            unsigned shards)
        : _families(families), _positions(shards, 0), _info(*this) {
        next();
    }

    metric_family_iterator(const metrics_families_per_shard& families,
            std::vector<size_t>&& positions)
        : _families(families), _positions(std::move(positions)), _info(*this) {
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

    uint32_t size() const {
        return _info._size;
    }

    const mi::metric_family_info& metadata() const {
        return *_info._family_info;
    }

    bool end() const {
        return _positions.empty() || _info.end();
    }

    void foreach_metric(std::function<void(const mi::metric_value&, const mi::metric_series_metadata&)>&& f) {
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
                const mi::metric_metadata_fifo& metrics_metadata = metadata.metrics;
                for (auto&& vm : boost::combine(values, metrics_metadata)) {
                    auto& value = boost::get<0>(vm);
                    auto& metric_metadata = boost::get<1>(vm);
                    f(value, metric_metadata);
                }
            }
        }
    }

};

void metric_family::foreach_metric(std::function<void(const mi::metric_value&, const mi::metric_series_metadata&)>&& f) {
    _iterator_state.foreach_metric(std::move(f));
}

class metric_family_range {
    metric_family_iterator _begin;
    metric_family_iterator _end;
public:
    metric_family_range(const metrics_families_per_shard& families) : _begin(families, smp::count),
        _end(metric_family_iterator(families, 0))
    {
    }

    metric_family_range(const metric_family_iterator& b, const metric_family_iterator& e) : _begin(b), _end(e)
    {
    }

    metric_family_iterator begin() const {
        return _begin;
    }

    metric_family_iterator end() const {
        return _end;
    }
};

metric_family_iterator metrics_families_per_shard::find_bound(const sstring& family_name, comp_function comp) const {
    std::vector<size_t> positions;
    positions.reserve(smp::count);

    for (auto& shard_info : _data) {
        std::vector<mi::metric_family_metadata>& metadata = *(shard_info->metadata);
        std::vector<mi::metric_family_metadata>::iterator it_b = boost::range::upper_bound(metadata, family_name, comp);
        positions.emplace_back(it_b - metadata.begin());
    }
    return metric_family_iterator(*this, std::move(positions));

}

metric_family_iterator metrics_families_per_shard::lower_bound(const sstring& family_name) const {
    return find_bound(family_name, [](const sstring& a, const mi::metric_family_metadata& b) {
        //sstring doesn't have a <= operator
        return a < b.mf.name || a == b.mf.name;
    });
}

metric_family_iterator metrics_families_per_shard::upper_bound(const sstring& family_name) const {
    return find_bound(family_name, [](const sstring& a, const mi::metric_family_metadata& b) {
        return a < b.mf.name;
    });
}

/*!
 * \brief a helper function to get metric family range
 * if metric_family_name is empty will return everything, if not, it will return
 * the range of metric family that match the metric_family_name.
 *
 * if prefix is true the match will be based on prefix
 */
metric_family_range get_range(const metrics_families_per_shard& mf, const sstring& metric_family_name, bool prefix) {
    if (metric_family_name == "") {
        return metric_family_range(mf);
    }
    auto upper_bount_prefix = metric_family_name;
    ++upper_bount_prefix.back();
    if (prefix) {
        return metric_family_range(mf.lower_bound(metric_family_name), mf.lower_bound(upper_bount_prefix));
    }
    auto lb = mf.lower_bound(metric_family_name);
    if (lb.end() || lb->name() != metric_family_name) {
        return metric_family_range(lb, lb); // just return an empty range
    }
    auto up = lb;
    ++up;
    return metric_family_range(lb, up);

}

void write_histogram(std::stringstream& s, const config& ctx, const sstring& name, const seastar::metrics::histogram& h, std::map<sstring, sstring> labels) noexcept {
    add_name(s, name + "_sum", labels, ctx);
    s << h.sample_sum << '\n';

    add_name(s, name + "_count", labels, ctx);
    s << h.sample_count  << '\n';

    auto& le = labels["le"];
    auto bucket = name + "_bucket";
    for (auto  i : h.buckets) {
         le = std::to_string(i.upper_bound);
        add_name(s, bucket, labels, ctx);
        s << i.count << '\n';
    }
    labels["le"] = "+Inf";
    add_name(s, bucket, labels, ctx);
    s << h.sample_count  << '\n';
}

void write_summary(std::stringstream& s, const config& ctx, const sstring& name, const seastar::metrics::histogram& h, std::map<sstring, sstring> labels) noexcept {
    if (h.sample_sum) {
        add_name(s, name + "_sum", labels, ctx);
        s << h.sample_sum  << '\n';
    }
    if (h.sample_count) {
        add_name(s, name + "_count", labels, ctx);
        s << h.sample_count  << '\n';
    }
    auto& le = labels["quantile"];
    for (auto  i : h.buckets) {
        le = std::to_string(i.upper_bound);
        add_name(s, name, labels, ctx);
        s << i.count  << '\n';
    }
}
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
    std::vector<std::string> _labels_to_aggregate_by;
    std::unordered_map<std::map<sstring, sstring>, seastar::metrics::impl::metric_value> _values;
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
    void add(const seastar::metrics::impl::metric_value& m, std::map<sstring, sstring> labels) noexcept {
        for (auto&& l : _labels_to_aggregate_by) {
            labels.erase(l);
        }
        std::unordered_map<std::map<sstring, sstring>, seastar::metrics::impl::metric_value>::iterator i = _values.find(labels);
        if ( i == _values.end()) {
            _values.emplace(std::move(labels), m);
        } else {
            i->second += m;
        }
    }
    const std::unordered_map<std::map<sstring, sstring>, seastar::metrics::impl::metric_value>& get_values() const noexcept {
        return _values;
    }
    bool empty() const noexcept {
        return _values.empty();
    }
};

void write_value_as_string(std::stringstream& s, const mi::metric_value& value) noexcept {
    std::string value_str;
    try {
        s << value;
    } catch (const std::range_error& e) {
        seastar_logger.debug("prometheus: write_value_as_string: {}: {}", s.str(), e.what());
        s << "NaN";
    } catch (...) {
        auto ex = std::current_exception();
        // print this error as it's ignored later on by `connection::start_response`
        seastar_logger.error("prometheus: write_value_as_string: {}: {}", s.str(), ex);
        std::rethrow_exception(std::move(ex));
    }
}

future<> write_text_representation(output_stream<char>& out, const config& ctx, const metric_family_range& m, bool show_help, bool enable_aggregation, std::function<bool(const mi::labels_type&)> filter) {
    return seastar::async([&ctx, &out, &m, show_help, enable_aggregation, filter] () mutable {
        bool found = false;
        std::stringstream s;
        for (metric_family& metric_family : m) {
            auto name = ctx.prefix + "_" + metric_family.name();
            found = false;
            metric_aggregate_by_labels aggregated_values(metric_family.metadata().aggregate_labels);
            bool should_aggregate = enable_aggregation && !metric_family.metadata().aggregate_labels.empty();
            metric_family.foreach_metric([&s, &out, &ctx, &found, &name, &metric_family, &aggregated_values, should_aggregate, show_help, &filter](const mi::metric_value& value, const mi::metric_series_metadata& value_info) mutable {
                s.clear();
                s.str("");
                if ((value_info.should_skip_when_empty() && value.is_empty()) || !filter(value_info.labels())) {
                    return;
                }
                if (!found) {
                    if (show_help && metric_family.metadata().d.str() != "") {
                        s << "# HELP " << name << " " <<  metric_family.metadata().d.str() << '\n';
                    }
                    s << "# TYPE " << name << " " << metric_family.metadata().type << '\n';
                    found = true;
                }
                if (should_aggregate) {
                    aggregated_values.add(value, value_info.labels());
                } else if (value.type() == mi::data_type::SUMMARY) {
                    write_summary(s, ctx, name, value.get_histogram(), value_info.labels());
                } else if (value.type() == mi::data_type::HISTOGRAM) {
                    write_histogram(s, ctx, name, value.get_histogram(), value_info.labels());
                } else {
                    add_name(s, name, value_info.labels(), ctx);
                    write_value_as_string(s, value);
                    s << '\n';
                }
                out.write(s.str()).get();
                thread::maybe_yield();
            });
            if (!aggregated_values.empty()) {
                for (auto&& h : aggregated_values.get_values()) {
                    s.clear();
                    s.str("");
                    if (h.second.type() == mi::data_type::HISTOGRAM) {
                        write_histogram(s, ctx, name, h.second.get_histogram(), h.first);
                    } else {
                        add_name(s, name, h.first, ctx);
                        write_value_as_string(s, h.second);
                        s << '\n';
                    }
                    out.write(s.str()).get();
                    thread::maybe_yield();
                }
            }
        }
    });
}

future<> write_protobuf_representation(output_stream<char>& out, const config& ctx, metric_family_range& m, bool enable_aggregation, std::function<bool(const mi::labels_type&)> filter) {
    return do_for_each(m, [&ctx, &out, enable_aggregation, filter=std::move(filter)](metric_family& metric_family) mutable {
        std::string s;
        google::protobuf::io::StringOutputStream os(&s);
        metric_aggregate_by_labels aggregated_values(metric_family.metadata().aggregate_labels);
        bool should_aggregate = enable_aggregation && !metric_family.metadata().aggregate_labels.empty();
        auto& name = metric_family.name();
        pm::MetricFamily mtf;
        bool empty_metric = true;
        mtf.set_name(fmt::format("{}_{}", ctx.prefix, name));
        mtf.mutable_metric()->Reserve(metric_family.size());
        metric_family.foreach_metric([&mtf, &ctx, &filter, &aggregated_values, &empty_metric, should_aggregate](const auto& value, const auto& value_info) {
            if ((value_info.should_skip_when_empty() && value.is_empty()) || !filter(value_info.labels())) {
                return;
            }
            if (should_aggregate) {
                aggregated_values.add(value, value_info.labels());
            } else {
                fill_metric(mtf, value, value_info.labels(), ctx);
                empty_metric = false;
            }
        });
        for (auto& [labels, value] : aggregated_values.get_values()) {
            fill_metric(mtf, value, labels, ctx);
            empty_metric = false;
        }
        if (empty_metric) {
            return make_ready_future<>();
        }
        if (!write_delimited_to(mtf, &os)) {
            seastar_logger.warn("Failed to write protobuf metrics");
        }
        return out.write(s);
    });
}

bool is_accept_protobuf(const std::string& accept) {
    std::vector<std::string> strs;
    boost::split(strs, accept, boost::is_any_of(","));
    for (auto i : strs) {
        boost::trim(i);
        if (boost::starts_with(i, "application/vnd.google.protobuf;")) {
            return true;
        }
    }
    return false;
}

class metrics_handler : public httpd::handler_base  {
    sstring _prefix;
    config _ctx;
    static std::function<bool(const mi::labels_type&)> _true_function;

    /*!
     * \brief tries to trim an asterisk from the end of the string
     * return true if an asterisk exists.
     */
    bool trim_asterisk(sstring& name) {
        if (name.size() && name.back() == '*') {
            name.resize(name.length() - 1);
            return true;
        }
        // Prometheus uses url encoding for the path so '*' is encoded as '%2A'
        if (boost::algorithm::ends_with(name, "%2A")) {
            // This assert is obviously true. It is in here just to
            // silence a bogus gcc warning:
            // https://gcc.gnu.org/bugzilla/show_bug.cgi?id=89337
            SEASTAR_ASSERT(name.length() >= 3);
            name.resize(name.length() - 3);
            return true;
        }
        return false;
    }
    /*!
     * \brief Return a filter function, based on the request
     *
     * A filter function filter what metrics should be included.
     * It returns true if a metric should be included, or false otherwise.
     * The filters are created from the request query parameters.
     */
    std::function<bool(const mi::labels_type&)> make_filter(const http::request& req) {
        std::unordered_map<sstring, std::regex> matcher;
        auto labels = mi::get_local_impl()->get_labels();
        for (auto&& qp : req.query_parameters) {
            if (labels.find(qp.first) != labels.end()) {
                matcher.emplace(qp.first, std::regex(qp.second.c_str()));
            }
        }
        return (matcher.empty()) ? _true_function : [matcher](const mi::labels_type& labels) {
            for (auto&& m : matcher) {
                auto l = labels.find(m.first);
                if (!std::regex_match((l == labels.end())? "" : l->second.c_str(), m.second)) {
                    return false;
                }
            }
            return true;
        };
    }

public:
    metrics_handler(config ctx) : _ctx(ctx) {}

    future<std::unique_ptr<http::reply>> handle(const sstring& path,
        std::unique_ptr<http::request> req, std::unique_ptr<http::reply> rep) override {
        auto is_protobuf_format = _ctx.allow_protobuf && is_accept_protobuf(req->get_header("Accept"));
        sstring metric_family_name = req->get_query_param("__name__");
        bool prefix = trim_asterisk(metric_family_name);
        bool show_help = req->get_query_param("__help__") != "false";
        bool enable_aggregation = req->get_query_param("__aggregate__") != "false";
        std::function<bool(const mi::labels_type&)> filter = make_filter(*req);
        rep->write_body(is_protobuf_format ? "proto" : "txt", [this, is_protobuf_format, metric_family_name, prefix, show_help, enable_aggregation, filter] (output_stream<char>&& s) {
            return do_with(metrics_families_per_shard(), output_stream<char>(std::move(s)),
                    [this, is_protobuf_format, prefix, &metric_family_name, show_help, enable_aggregation, filter] (metrics_families_per_shard& families, output_stream<char>& s) mutable {
                return get_map_value(families).then([&s, &families, this, is_protobuf_format, prefix, &metric_family_name, show_help, enable_aggregation, filter]() mutable {
                    return do_with(get_range(families, metric_family_name, prefix),
                            [&s, this, is_protobuf_format, show_help, enable_aggregation, filter](metric_family_range& m) {
                        return (is_protobuf_format) ?  write_protobuf_representation(s, _ctx, m, enable_aggregation, filter) :
                                write_text_representation(s, _ctx, m, show_help, enable_aggregation, filter);
                    });
                }).finally([&s] () mutable {
                    return s.close();
                });
            });
        });
        return make_ready_future<std::unique_ptr<http::reply>>(std::move(rep));
    }
};

std::function<bool(const mi::labels_type&)> metrics_handler::_true_function = [](const mi::labels_type&) {
    return true;
};

future<> add_prometheus_routes(httpd::http_server& server, config ctx) {
    server._routes.put(httpd::GET, "/metrics", new metrics_handler(ctx));
    return make_ready_future<>();
}

future<> add_prometheus_routes(distributed<httpd::http_server>& server, config ctx) {
    return server.invoke_on_all([ctx](httpd::http_server& s) {
        return add_prometheus_routes(s, ctx);
    });
}

future<> start(httpd::http_server_control& http_server, config ctx) {
    return add_prometheus_routes(http_server.server(), ctx);
}

}
}
