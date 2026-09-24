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

#include <seastar/core/internal/fmt.hh>
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
#include "proto/metrics2.pb.h"

#include <seastar/core/future.hh>
#include <seastar/core/iostream.hh>
#include <seastar/core/metrics.hh>
#include <seastar/core/metrics_api.hh>
#include <seastar/core/prometheus.hh>
#include <seastar/core/scollectd.hh>
#include <seastar/http/function_handlers.hh>

#include "prometheus-impl.hh"

#include <boost/algorithm/string/replace.hpp>
#include <boost/algorithm/string.hpp>
#include <seastar/core/thread.hh>
#include <seastar/core/loop.hh>
#include <seastar/core/lowres_clock.hh>
#include <seastar/util/std-compat.hh>
#include <seastar/util/assert.hh>
#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <ranges>
#include <span>
#include <regex>
#include <string_view>
#include <type_traits>

#ifdef SEASTAR_ASAN_ENABLED
#include <sanitizer/lsan_interface.h>
#endif

using namespace std::literals;

template<>
struct fmt::formatter<seastar::metrics::impl::metric_value> {
    constexpr auto parse (format_parse_context& ctx) {
        return ctx.begin();
    }

    constexpr auto format(const seastar::metrics::impl::metric_value& v, auto& ctx) const {
        switch (v.type()) {
        case seastar::metrics::impl::data_type::GAUGE:
        case seastar::metrics::impl::data_type::REAL_COUNTER:
            return format_to(ctx.out(), FMT_COMPILE("{:.6f}"), v.d());
            break;
        case seastar::metrics::impl::data_type::COUNTER:
            return format_to(ctx.out(), FMT_COMPILE("{}"), v.i());
            break;
        case seastar::metrics::impl::data_type::HISTOGRAM:
        case seastar::metrics::impl::data_type::SUMMARY:
            // handled with a different code path
            return ctx.out();
        }
        assert(false);
        __builtin_unreachable();
    }
};

namespace seastar {

extern seastar::logger seastar_logger;

constexpr std::string_view to_string(seastar::metrics::impl::data_type t) {
    switch (t) {
    case seastar::metrics::impl::data_type::GAUGE:
        return "gauge";
    case seastar::metrics::impl::data_type::COUNTER:
    case seastar::metrics::impl::data_type::REAL_COUNTER:
        return "counter";
    case seastar::metrics::impl::data_type::HISTOGRAM:
        return "histogram";
    case seastar::metrics::impl::data_type::SUMMARY:
        return "summary";
    };
    return "untyped";
};

namespace prometheus {
namespace pm = io::prometheus::client;

namespace mi = metrics::impl;

using mi::labels_type;
using write_body_args = details::write_body_args;

namespace {

// A wrapper around a std::vector for efficient formatting and appending
// of text data. Just using a std::vector is much slower.
// Inspired by log_buf.
class fmt_buf {
    char* _current;
    std::vector<char> buffer;
private:

    const char* buf_end() const noexcept {
        return buffer.data() + buffer.size();
    }

    auto& realloc_buffer_and_append(char c) {
        auto current_size = size();  // Save size before resizing
        buffer.resize(buffer.size() * 2);
        _current = buffer.data() + current_size;
        *_current++ = c;
        return *this;
    }

public:
    static constexpr size_t initial_size = 1024;

    fmt_buf() {
        buffer.resize(initial_size);
        _current = buffer.data();
    }

    fmt_buf(const fmt_buf&) = delete;
    fmt_buf(fmt_buf&&) = delete;
    fmt_buf& operator=(const fmt_buf&) = delete;
    fmt_buf& operator=(fmt_buf&&) = delete;

    /// Clear the buffer, setting its position back to the start, but does not
    /// free any buffers (after this called, size is zero, capacity is unchanged).
    /// Any existing iterators are invalidated.
    void clear() { _current = data(); }

    /// The amount of data written so far.
    size_t size() const noexcept { return _current - data(); }

    /// The remaining space
    size_t remaining() const noexcept { return buf_end() - _current; }

    const char* data() const noexcept { return buffer.data(); }
    char* data() noexcept { return buffer.data(); }

    auto& append(char c) noexcept {
        if (_current == buf_end()) [[unlikely]] {
            return realloc_buffer_and_append(c);
        }
        *_current++ = c;
        return *this;
    }

    fmt_buf& append(std::string_view str) {
        if (str.size() <= remaining()) [[likely]] {
            std::copy(str.begin(), str.end(), _current);
            _current += str.size();
        } else {
            append_slowpath(str);
        }
        return *this;
    }

    fmt_buf& operator<<(std::string_view str) {
        return append(str);
    }

    void append_slowpath(std::string_view sv) {
        // This is taken very infrequently, as we (a) size the buffer generously
        // to start and (b) keep using the same buffer for the entire request
        // but flush it every metric, so so once it grows larger (if it needs
        // to) that space can be reused by subsequent metrics, so the total number
        // of appends is effectively capped per request.
        // Therefore, we just do the simplest thing here which is to append char
        // by char (which internally handles the resize).
        for (auto c : sv) {
            append(c);
        }
    }

    // removes last character added (buffer must not be empty)
    void pop_back() {
        --_current;
    }

    // the last character added (buffer must not be empty)
    char back() {
        return *(_current - 1);
    }

    std::string_view str() const {
        return {data(), size()};
    }

    class inserter_iterator {
        fmt_buf* _buf;
    public:
        using iterator_category = std::output_iterator_tag;
        using difference_type = std::ptrdiff_t;
        using value_type = void;
        using pointer = void;
        using reference = void;

        explicit inserter_iterator(fmt_buf& buf) noexcept : _buf(&buf) { }

        void operator=(char c) noexcept {
            _buf->append(c);
        }

        inserter_iterator& operator*() noexcept { return *this; }
        inserter_iterator& operator++() noexcept { return *this; }
        inserter_iterator operator++(int) noexcept { return *this; }
    };

    /// Create an output iterator which allows writing into the buffer.
    inserter_iterator back_insert_begin() noexcept { return inserter_iterator(*this); }
};
}

using buf_t = fmt_buf;
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
        label->set_value(std::string(value.value()));
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

static inline bool is_internal(const sstring& name) {
    if (auto cstr = name.c_str(); cstr[0] == '_' && cstr[1] == '_') [[unlikely]] {
        return true;
    }
    return false;
}

[[gnu::always_inline]]
static inline void write_label(buf_t& s, std::string_view key, std::string_view value) {
    s << key << "=\"";
    s << value;
    s << "\",";
};

struct no_label {};

template <typename Extra = no_label>
static void write_name_and_labels(buf_t& buf, std::string_view name, auto suffix, const labels_type& labels, const config& ctx, Extra extra = {}) {

    // extra label can be injected in by the caller, unfortunately prometheus
    // requires labels to be sorted by name, so we have to jump through some
    // hoops to do that.
    constexpr bool has_extra = !std::is_same_v<Extra, no_label>;

    buf << name << suffix << "{";
    if (ctx.label) [[unlikely]] {
        write_label(buf, ctx.label->key(), ctx.label->value());
    }

    bool wrote_extra = false;

    for (auto& l : labels) {
        std::string_view label_name = l.first;
        if constexpr (has_extra) {
            if (!wrote_extra && extra.name < label_name) {
                write_label(buf, extra.name, extra.value);
                wrote_extra = true;
            }
        }
        if (!is_internal(l.first)) [[likely]] {
            write_label(buf, label_name, l.second.value());
        }
    }

    if constexpr (has_extra) {
        if (!wrote_extra) {
            write_label(buf, extra.name, extra.value);
        }
    }

    if (buf.back() == ',') {
        buf.pop_back();
    }

    buf.append("} ");
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

static future<metrics_families_per_shard> get_map_value() {
    metrics_families_per_shard vec;
    vec.resize(this_smp_shard_count());
    co_await parallel_for_each(std::views::iota(0u, this_smp_shard_count()), [&vec] (auto cpu) {
        return smp::submit_to(cpu, [] {
            return mi::get_values();
        }).then([&vec, cpu] (auto res) {
            vec[cpu] = std::move(res);
        });
    });
    co_return vec;
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

    // Like foreach_metric(), but also passes the location of each metric: its
    // shard, the position of the family in the shard's metadata, and the
    // position of the metric in the family.
    template <typename Func>
    void foreach_metric_with_location(Func&& f);

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
        for (auto&& [pos_in_metric_per_shard, metric_family] : std::views::zip(_positions, _families)) {
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
        for (auto&& [pos_in_metric_per_shard, metric_family] : std::views::zip(_positions, _families)) {
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
                for (auto&& [value, metric_metadata] : std::views::zip(values, metrics_metadata)) {
                    f(value, metric_metadata);
                }
            }
        }
    }


    template <typename Func>
    void foreach_metric_with_location(Func&& f) {
        unsigned shard = 0;
        for (auto&& [pos_in_metric_per_shard, metric_family] : std::views::zip(_positions, _families)) {
            auto this_shard = shard++;
            if (pos_in_metric_per_shard >= metric_family->metadata->size()) {
                continue;
            }
            auto& metadata = metric_family->metadata->at(pos_in_metric_per_shard);
            if (metadata.mf.name == name()) {
                const mi::value_vector& values = metric_family->values[pos_in_metric_per_shard];
                uint32_t index = 0;
                for (auto&& [value, metric_metadata] : std::views::zip(values, metadata.metrics)) {
                    f(this_shard, uint32_t(pos_in_metric_per_shard), index++, value, metric_metadata);
                }
            }
        }
    }
};

void metric_family::foreach_metric(std::function<void(const mi::metric_value&, const mi::metric_series_metadata&)>&& f) {
    _iterator_state.foreach_metric(std::move(f));
}

template <typename Func>
void metric_family::foreach_metric_with_location(Func&& f) {
    _iterator_state.foreach_metric_with_location(std::forward<Func>(f));
}

class metric_family_range {
    metric_family_iterator _begin;
    metric_family_iterator _end;
public:
    metric_family_range(const metrics_families_per_shard& families) : _begin(families, this_smp_shard_count()),
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
    positions.reserve(this_smp_shard_count());

    for (auto& shard_info : _data) {
        std::vector<mi::metric_family_metadata>& metadata = *(shard_info->metadata);
        std::vector<mi::metric_family_metadata>::iterator it_b = std::upper_bound(metadata.begin(), metadata.end(), family_name, comp);
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


struct extra_label {
    std::string_view name, value;
};

/*
 * Text representation templates
 *
 * Formatting the text representation is expensive: every line repeats the
 * metric name and all of its labels, while only the value changes between
 * requests. So, like a prepared statement, the text is formatted once into a
 * template with the values left out, and each request only computes and
 * formats the values.
 *
 * A template is built from a snapshot of the metrics (the values collected
 * from all shards), and can be used for any later snapshot with the same
 * shape, as indicated by the metrics generation of every shard. A template
 * is split into:
 *
 *  - families, each with a header (the HELP and TYPE lines) and series
 *  - series, each a metric (or with aggregation, the sum of several metrics),
 *    referring to its metrics' locations in the snapshot
 *  - lines, one per output line; a scalar series has a single line, while
 *    histograms and summaries have a line per bucket and more.
 *
 A few parts of the output depend on the values and not only on the shape,
 * so are verified against the snapshot, and a mismatch invalidates the
 * template:
 *
 *  - the buckets of histograms and summaries.
 *  - summary sums and counts are omitted while they're zero, so are part
 *    of the template only if they were non-zero when it was built (and
 *    from then on reported even if zero).
 *
 * Using a template has two steps: evaluate() computes the value of each
 * line from a snapshot (or detects that the snapshot doesn't fit the
 * template), and render_text() writes the output.
 */

// The value of an output line
struct line_value {
    enum class kind : uint8_t {
        fixed,   // d, formatted as {:.6f}
        general, // d, formatted as {:g}
        int64,   // i
        uint64,  // u
        nan,     // a counter that isn't representable as an integer
        empty,   // no value (aggregated summaries)
    };
    kind k = kind::empty;
    union {
        double d;
        int64_t i;
        uint64_t u;
    };

    line_value() noexcept : u(0) {}
    line_value(kind k, double d) noexcept : k(k), d(d) {}
    line_value(int64_t i) noexcept : k(kind::int64), i(i) {}
    line_value(uint64_t u) noexcept : k(kind::uint64), u(u) {}
    explicit line_value(kind k) noexcept : k(k), u(0) {}
};

// The longest value format_value() writes: {:.6f} of -DBL_MAX
constexpr size_t max_formatted_value_size = 320;

// Writes the formatted value to out, returning the end of the output
static char* format_value(const line_value& v, char* out) noexcept {
    switch (v.k) {
    case line_value::kind::fixed:
        return fmt::format_to(out, FMT_COMPILE("{:.6f}"), v.d);
    case line_value::kind::general:
        return fmt::format_to(out, FMT_COMPILE("{:g}"), v.d);
    case line_value::kind::int64:
        return fmt::format_to(out, FMT_COMPILE("{}"), v.i);
    case line_value::kind::uint64:
        return fmt::format_to(out, FMT_COMPILE("{}"), v.u);
    case line_value::kind::nan:
        return std::copy_n("NaN", 3, out);
    case line_value::kind::empty:
        return out;
    }
    __builtin_unreachable();
}

// The location of a metric in a snapshot
struct metric_location {
    uint32_t shard;
    uint32_t family; // position of the family in the shard's metadata
    uint32_t index;  // position of the metric in the family
};

struct series_template {
    mi::data_type type;
    bool aggregated;
    // summaries: whether the sum and count are reported
    bool has_sum;
    bool has_count;
    uint32_t layout;       // histograms and summaries: index into text_template::layouts
    uint32_t first_source; // index into text_template::sources
    uint32_t nr_sources;
    uint32_t first_line;   // index into text_template::lines
    uint32_t nr_lines;
};

struct line_template {
    // The name and labels, followed by a space, in text_template::literals
    uint32_t begin;
    uint32_t end;
};

struct family_template {
    // The HELP and TYPE lines, in text_template::literals
    uint32_t header_begin;
    uint32_t header_end;
    uint32_t first_line;
    uint32_t end_line;
};

struct text_template {
    std::string literals;
    std::vector<family_template> families;
    std::vector<series_template> series;
    std::vector<metric_location> sources;
    std::vector<line_template> lines;
    // Bucket upper bounds of histograms and summaries
    std::vector<std::vector<double>> layouts;
};

// The values of a template's lines, computed from a snapshot
using text_values = std::vector<line_value>;

// Computes the value of a series from a snapshot. value points either into
// the snapshot or to storage. Returns false if the snapshot doesn't fit the
// template.
static bool get_series_value(const series_template& s, std::span<const metric_location> sources,
        const metrics_families_per_shard& snapshot, const mi::metric_value*& value, mi::metric_value& storage) {
    auto get = [&] (const metric_location& l) -> const mi::metric_value& {
        return snapshot[l.shard]->values[l.family][l.index];
    };
    if (!s.aggregated) {
        value = &get(sources[0]);
        return value->type() == s.type;
    }
    for (auto& l : sources) {
        auto& v = get(l);
        if (v.type() != s.type) [[unlikely]] {
            return false;
        }
        if (&l == &sources[0]) {
            storage = v;
        } else {
            try {
                storage += v;
            } catch (const std::out_of_range&) {
                // histograms with different buckets
                return false;
            }
        }
    }
    value = &storage;
    return true;
}

static bool same_layout(const metrics::histogram& h, const std::vector<double>& layout) noexcept {
    return h.buckets.size() == layout.size()
        && std::ranges::equal(h.buckets, layout, std::equal_to<>(), &metrics::histogram_bucket::upper_bound);
}

// Sets the values of a series' lines. Returns false if the value doesn't fit
// the template.
static bool set_line_values(const text_template& t, const series_template& s, const mi::metric_value& v, line_value* out) {
    using kind = line_value::kind;
    switch (s.type) {
    case mi::data_type::COUNTER:
        try {
            out[0] = line_value(int64_t(v.i()));
        } catch (const std::range_error&) {
            out[0] = line_value(kind::nan);
        }
        return true;
    case mi::data_type::GAUGE:
    case mi::data_type::REAL_COUNTER:
        out[0] = line_value(kind::fixed, v.d());
        return true;
    case mi::data_type::HISTOGRAM: {
        auto& h = v.get_histogram();
        if (!same_layout(h, t.layouts[s.layout])) {
            return false;
        }
        *out++ = line_value(kind::general, h.sample_sum);
        *out++ = line_value(h.sample_count);
        for (auto& b : h.buckets) {
            *out++ = line_value(b.count);
        }
        *out++ = line_value(h.sample_count);
        return true;
    }
    case mi::data_type::SUMMARY: {
        if (s.aggregated) {
            out[0] = line_value(kind::empty);
            return true;
        }
        auto& h = v.get_histogram();
        if (!same_layout(h, t.layouts[s.layout]) || (h.sample_sum && !s.has_sum) || (h.sample_count && !s.has_count)) {
            return false;
        }
        if (s.has_sum) {
            *out++ = line_value(kind::general, h.sample_sum);
        }
        if (s.has_count) {
            *out++ = line_value(h.sample_count);
        }
        for (auto& b : h.buckets) {
            *out++ = line_value(b.count);
        }
        return true;
    }
    }
    return false;
}

// Computes the values of all lines of the template from the snapshot.
// Returns false if the snapshot doesn't fit the template.
// Must run in a seastar::thread.
static bool evaluate(const text_template& t, const metrics_families_per_shard& snapshot, text_values& out) {
    out.resize(t.lines.size());
    mi::metric_value storage;
    for (auto& s : t.series) {
        const mi::metric_value* v = nullptr;
        auto sources = std::span(t.sources).subspan(s.first_source, s.nr_sources);
        if (!get_series_value(s, sources, snapshot, v, storage) || !set_line_values(t, s, *v, out.data() + s.first_line)) {
            return false;
        }
        thread::maybe_yield();
    }
    return true;
}

// Builds a template from a snapshot.
// Must run in a seastar::thread.
static text_template build_text_template(const metric_family_range& m, const metrics_families_per_shard& snapshot,
        const config& ctx, const write_body_args& args) {
    text_template t;
    buf_t buf;
    std::map<std::vector<double>, uint32_t> layout_index;

    auto offset = [&] {
        if (t.literals.size() > std::numeric_limits<uint32_t>::max()) {
            throw std::length_error("prometheus: text representation too large");
        }
        return uint32_t(t.literals.size());
    };
    auto intern_layout = [&] (const metrics::histogram& h) {
        std::vector<double> layout;
        layout.reserve(h.buckets.size());
        for (auto& b : h.buckets) {
            layout.push_back(b.upper_bound);
        }
        auto [it, inserted] = layout_index.try_emplace(std::move(layout), t.layouts.size());
        if (inserted) {
            t.layouts.push_back(it->first);
        }
        return it->second;
    };

    struct contributor {
        metric_location location;
        const mi::metric_value* value;
        const labels_type* labels;
    };
    std::vector<contributor> contributors;
    std::vector<metric_location> locations;

    for (metric_family& mf : m) {
        if (!args.family_filter(mf.name())) {
            continue;
        }
        contributors.clear();
        mf.foreach_metric_with_location([&] (unsigned shard, uint32_t family, uint32_t index,
                const mi::metric_value& value, const mi::metric_series_metadata& md) {
            if (args.filter(md.labels())) {
                contributors.push_back({{shard, family, index}, &value, &md.labels()});
            }
        });
        if (contributors.empty()) {
            continue;
        }
        auto name = ctx.prefix + "_" + mf.name();
        auto& metadata = mf.metadata();

        family_template ft;
        ft.header_begin = offset();
        buf.clear();
        if (args.show_help && metadata.d.str() != "") {
            buf << "# HELP " << name << " " << metadata.d.str() << "\n";
        }
        buf << "# TYPE " << name << " " << to_string(metadata.type) << "\n";
        t.literals.append(buf.str());
        ft.header_end = offset();
        ft.first_line = t.lines.size();

        // sample: any of the series' values, for its type
        auto add_series = [&] (const labels_type& labels, std::span<const metric_location> sources, bool aggregated,
                const mi::metric_value& sample) {
            series_template s{
                .type = sample.type(),
                .aggregated = aggregated,
                .has_sum = false,
                .has_count = false,
                .layout = 0,
                .first_source = uint32_t(t.sources.size()),
                .nr_sources = uint32_t(sources.size()),
                .first_line = uint32_t(t.lines.size()),
                .nr_lines = 0,
            };
            t.sources.insert(t.sources.end(), sources.begin(), sources.end());
            const mi::metric_value* v = nullptr;
            mi::metric_value storage;
            if (!get_series_value(s, sources, snapshot, v, storage)) {
                throw std::runtime_error(fmt::format("prometheus: inconsistent values in metric family {}", mf.name()));
            }
            auto add_line = [&] (std::string_view suffix, auto extra) {
                buf.clear();
                write_name_and_labels(buf, name, suffix, labels, ctx, extra);
                auto begin = offset();
                t.literals.append(buf.str());
                t.lines.push_back({begin, offset()});
            };
            switch (s.type) {
            case mi::data_type::HISTOGRAM: {
                auto& h = v->get_histogram();
                s.layout = intern_layout(h);
                add_line("_sum", no_label{});
                add_line("_count", no_label{});
                for (auto& b : h.buckets) {
                    add_line("_bucket", extra_label{"le", fmt::format(FMT_COMPILE("{:f}"), b.upper_bound)});
                }
                add_line("_bucket", extra_label{"le", "+Inf"});
                break;
            }
            case mi::data_type::SUMMARY:
                if (aggregated) {
                    add_line("", no_label{});
                    break;
                } else {
                    auto& h = v->get_histogram();
                    s.layout = intern_layout(h);
                    s.has_sum = h.sample_sum != 0;
                    s.has_count = h.sample_count != 0;
                    if (s.has_sum) {
                        add_line("_sum", no_label{});
                    }
                    if (s.has_count) {
                        add_line("_count", no_label{});
                    }
                    for (auto& b : h.buckets) {
                        add_line("", extra_label{"quantile", fmt::format(FMT_COMPILE("{:f}"), b.upper_bound)});
                    }
                }
                break;
            default:
                add_line("", no_label{});
                break;
            }
            s.nr_lines = t.lines.size() - s.first_line;
            t.series.push_back(s);
            thread::maybe_yield();
        };

        if (args.enable_aggregation && !metadata.aggregate_labels.empty()) {
            // Group the metrics by the labels which aren't aggregated. The
            // groups are output in the same order metric_aggregate_by_labels
            // uses.
            struct group {
                labels_type labels;
                std::vector<metric_location> sources;
                const mi::metric_value* sample;
            };
            std::vector<group> groups;
            std::unordered_map<label_key, uint32_t> group_index;
            label_key key;
            for (auto& c : contributors) {
                key.construct(*c.labels, metadata.aggregate_labels);
                auto [it, inserted] = group_index.try_emplace(key, groups.size());
                if (inserted) {
                    labels_type labels;
                    for (auto& l : *c.labels) {
                        if (!internal::is_aggregated(metadata.aggregate_labels, l.first)) {
                            labels.insert(l);
                        }
                    }
                    groups.push_back({std::move(labels), {}, c.value});
                }
                groups[it->second].sources.push_back(c.location);
            }
            for (auto& [_, i] : group_index) {
                add_series(groups[i].labels, groups[i].sources, true, *groups[i].sample);
            }
        } else {
            for (auto& c : contributors) {
                add_series(*c.labels, std::span(&c.location, 1), false, *c.value);
            }
        }
        ft.end_line = t.lines.size();
        t.families.push_back(ft);
    }
    return t;
}

// Writes the text representation of a template with the given values.
// Must run in a seastar::thread.
static void render_text(const text_template& t, const text_values& values, output_stream<char>& out) {
    static constexpr size_t chunk_size = 128 * 1024;
    temporary_buffer<char> buf;
    char* p = nullptr;
    char* end = nullptr;
    auto flush = [&] {
        if (p != buf.get()) {
            buf.trim(p - buf.get());
            out.write(std::move(buf)).get();
        }
    };
    // Makes room for at least n bytes
    auto reserve = [&] (size_t n) {
        if (size_t(end - p) < n) [[unlikely]] {
            flush();
            buf = temporary_buffer<char>(std::max(n, chunk_size));
            p = buf.get_write();
            end = p + buf.size();
        }
    };
    auto append = [&] (uint32_t begin, uint32_t end) {
        p = std::copy(t.literals.data() + begin, t.literals.data() + end, p);
    };

    for (auto& f : t.families) {
        reserve(f.header_end - f.header_begin);
        append(f.header_begin, f.header_end);
        for (auto li = f.first_line; li != f.end_line; ++li) {
            auto& v = values[li];
            auto& l = t.lines[li];
            reserve(l.end - l.begin + max_formatted_value_size + 1);
            append(l.begin, l.end);
            p = format_value(v, p);
            *p++ = '\n';
            thread::maybe_yield();
        }
    }
    flush();
}

// Identifies a template: everything which affects the output except the
// values: the configuration, the request's options and filters, and the
// metrics' shape (identified by the shards' generations)
struct template_key {
    sstring prefix;
    std::optional<std::pair<sstring, sstring>> label;
    bool show_help;
    bool enable_aggregation;
    details::filter_key filters;
    // The metrics generation of every shard
    std::vector<uint64_t> generations;

    bool operator==(const template_key&) const = default;
};

struct template_key_hash {
    size_t operator()(const template_key& k) const noexcept {
        std::hash<sstring> h;
        size_t seed = h(k.prefix);
        if (k.label) {
            boost::hash_combine(seed, h(k.label->first));
            boost::hash_combine(seed, h(k.label->second));
        }
        boost::hash_combine(seed, k.show_help);
        boost::hash_combine(seed, k.enable_aggregation);
        for (auto& n : k.filters.names) {
            boost::hash_combine(seed, h(n.name));
            boost::hash_combine(seed, n.is_prefix);
        }
        for (auto& [label, expr] : k.filters.labels) {
            boost::hash_combine(seed, h(label));
            boost::hash_combine(seed, h(expr));
        }
        boost::hash_range(seed, k.generations.begin(), k.generations.end());
        return seed;
    }
};

static template_key make_template_key(const config& ctx, const write_body_args& args, const metrics_families_per_shard& families) {
    return {
        .prefix = ctx.prefix,
        .label = ctx.label ? std::make_optional(std::pair(sstring(ctx.label->key()), sstring(ctx.label->value()))) : std::nullopt,
        .show_help = args.show_help,
        .enable_aggregation = args.enable_aggregation,
        .filters = *args.cache_key,
        .generations = families | std::views::transform(&mi::values_copy::generation)
                | std::ranges::to<std::vector>(),
    };
}

/*
 * A cache of text representation templates
 *
 * Each shard keeps its own cache. Templates are keyed by everything which
 * affects the output except the values: the configuration, the request's
 * filters and options, and the metrics generation of every shard.
 * Templates expire some time after they were last used, so that templates
 * of requests which are no longer made are dropped, while those in use are
 * only rebuilt when they no longer fit the metrics.
 */
static std::atomic<lowres_clock::duration::rep> template_ttl{std::chrono::duration_cast<lowres_clock::duration>(std::chrono::minutes(5)).count()};

// The expiry time of a template used now
static lowres_clock::time_point template_expiry() {
    return lowres_clock::now() + lowres_clock::duration(template_ttl.load(std::memory_order_relaxed));
}

// A template, possibly shared by another shard (see share_template())
using shared_text_template = foreign_ptr<lw_shared_ptr<const text_template>>;

class template_cache {
public:
    struct entry {
        lowres_clock::time_point expiry;
        lw_shared_ptr<shared_text_template> tmpl;
    };
private:
    // Limits the memory used by the cache, in case of requests with
    // many distinct filters.
    static constexpr size_t max_entries = 16;

    // Templates of older generations are no longer found once the metrics
    // change, and are dropped when they expire or are evicted.
    std::unordered_map<template_key, entry, template_key_hash> _entries;

    void expire(lowres_clock::time_point now) {
        std::erase_if(_entries, [now] (const auto& e) { return e.second.expiry <= now; });
    }
public:
    details::text_cache_stats stats;

    template_cache() = default;
    template_cache(const template_cache&) = delete;

    // The cache is destroyed when its thread exits, after the reactor
    // stopped, so templates owned by other shards can't be released, since
    // that requires messaging them. Leak them instead.
    ~template_cache() {
        for (auto& [_, e] : _entries) {
            if (e.tmpl->get_owner_shard() != this_shard_id()) {
                [[maybe_unused]] auto leaked = new shared_text_template(std::move(*e.tmpl));
#ifdef SEASTAR_ASAN_ENABLED
                __lsan_ignore_object(leaked);
#endif
            }
        }
    }

    lw_shared_ptr<shared_text_template> find(const template_key& key) {
        expire(lowres_clock::now());
        auto it = _entries.find(key);
        if (it == _entries.end()) {
            return nullptr;
        }
        it->second.expiry = template_expiry();
        return it->second.tmpl;
    }

    void insert(const template_key& key, entry e) {
        expire(lowres_clock::now());
        auto it = _entries.find(key);
        if (it != _entries.end()) {
            it->second = std::move(e);
            return;
        }
        if (_entries.size() >= max_entries) {
            _entries.erase(std::ranges::min_element(_entries, std::less<>(), [] (const auto& e) { return e.second.expiry; }));
        }
        _entries.emplace(key, std::move(e));
    }

    size_t size() const noexcept {
        return _entries.size();
    }

    const text_template* any_template() const noexcept {
        return _entries.empty() ? nullptr : _entries.begin()->second.tmpl->get();
    }

    void clear() noexcept {
        _entries.clear();
    }
};

static template_cache& local_template_cache() {
    static thread_local template_cache cache;
    return cache;
}

// Shares a template built by this shard with the other shards, which likely
// receive the same requests. Each NUMA node gets its own copy, allocated by
// one of its shards, which is shared by all of the node's shards.
static std::optional<std::vector<unsigned>> numa_node_mapping_for_tests;

static future<> share_template(template_key key, lowres_clock::time_point expiry,
        lw_shared_ptr<const text_template> tmpl) {
    auto mapping = numa_node_mapping_for_tests ? std::span<const unsigned>(*numa_node_mapping_for_tests)
            : this_smp().shard_to_numa_node_mapping();
    auto node_of = [mapping] (unsigned shard) {
        return shard < mapping.size() ? mapping[shard] : 0;
    };
    const auto origin = this_shard_id();
    std::map<unsigned, std::vector<unsigned>> nodes;
    for (auto shard : std::views::iota(0u, this_smp_shard_count())) {
        if (shard != origin) {
            nodes[node_of(shard)].push_back(shard);
        }
    }
    // For each shard, a pointer to its node's copy of the template, owned
    // by the shard which allocated it
    std::vector<shared_text_template> copies(this_smp_shard_count());
    co_await parallel_for_each(nodes, [&] (this auto self, const auto& node) -> future<> {
        auto& [id, shards] = node;
        if (id == node_of(origin)) {
            for (auto shard : shards) {
                copies[shard] = make_foreign(tmpl);
            }
            co_return;
        }
        co_await smp::submit_to(shards.front(), [&] {
            auto copy = make_lw_shared<const text_template>(*tmpl);
            for (auto shard : shards) {
                copies[shard] = make_foreign(copy);
            }
        });
    });
    co_await smp::invoke_on_all([&] {
        if (this_shard_id() != origin) {
            local_template_cache().insert(key, {expiry,
                    make_lw_shared<shared_text_template>(std::move(copies[this_shard_id()]))});
        }
    });
}

details::family_filter_t details::make_family_filter(std::vector<details::name_filter> filters, std::string_view prefix) {
    if (filters.empty()) {
        return [](std::string_view) { return true; };
    }
    // Strip prefix from filter names if present
    if (!prefix.empty()) {
        auto prefix_with_underscore = sstring(prefix) + "_";
        for (auto& f : filters) {
            if (f.name.starts_with(prefix_with_underscore)) {
                f.name = f.name.substr(prefix_with_underscore.size());
            }
        }
    }
    return [filters = std::move(filters)](std::string_view family_name) {
        for (const auto& f : filters) {
            bool match = f.is_prefix ? family_name.starts_with(f.name) : family_name == f.name;
            if (match) {
                return true;
            }
        }
        return false;
    };
}

struct write_context {
    output_stream<char>& out;
    const config& ctx;
    const metrics_families_per_shard& families;
    const metric_family_range m;
    const write_body_args args;

    future<> write_text_representation();
    future<> write_protobuf_representation();
};

future<> write_context::write_text_representation() {
    return seastar::async([this] {
        auto& cache = local_template_cache();
        lw_shared_ptr<shared_text_template> t;
        std::optional<template_key> key;
        if (args.cache_key) {
            key = make_template_key(ctx, args, families);
            t = cache.find(*key);
        }
        text_values values;
        future<> shared = make_ready_future<>();
        if (t) {
            if (evaluate(**t, families, values)) {
                ++cache.stats.hits;
            } else {
                ++cache.stats.invalidations;
                t = nullptr;
            }
        }
        if (!t) {
            ++cache.stats.builds;
            auto built = make_lw_shared<const text_template>(build_text_template(m, families, ctx, args));
            if (!evaluate(*built, families, values)) {
                throw std::logic_error("prometheus: template doesn't fit the snapshot it was built from");
            }
            t = make_lw_shared<shared_text_template>(make_foreign(built));
            if (key) {
                auto expiry = template_expiry();
                cache.insert(*key, {expiry, t});
                // Shares the template in the background of rendering
                shared = share_template(std::move(*key), expiry, std::move(built))
                        .handle_exception([] (std::exception_ptr ex) {
                    seastar_logger.warn("prometheus: failed to share a text template: {}", ex);
                });
            }
        }
        std::exception_ptr ex;
        try {
            render_text(**t, values, out);
        } catch (...) {
            ex = std::current_exception();
        }
        shared.get();
        if (ex) {
            std::rethrow_exception(std::move(ex));
        }
    });
}

future<> write_context::write_protobuf_representation() {
    return do_for_each(m, [this](metric_family& metric_family) mutable {
        if (!args.family_filter(metric_family.name())) {
            return make_ready_future<>();
        }
        std::string s;
        google::protobuf::io::StringOutputStream os(&s);
        metric_aggregate_by_labels aggregated_values(metric_family.metadata().aggregate_labels);
        bool should_aggregate = args.enable_aggregation && !metric_family.metadata().aggregate_labels.empty();
        auto& name = metric_family.name();
        pm::MetricFamily mtf;
        bool empty_metric = true;
        mtf.set_name(fmt::format("{}_{}", ctx.prefix, name));
        mtf.mutable_metric()->Reserve(metric_family.size());
        metric_family.foreach_metric([this, &mtf, &aggregated_values, &empty_metric, should_aggregate](const auto& value, const auto& value_info) {
            if (!args.filter(value_info.labels())) {
                return;
            }
            if (should_aggregate) {
                aggregated_values.add(value, value_info.labels());
            } else {
                fill_metric(mtf, value, value_info.labels(), ctx);
                empty_metric = false;
            }
        });
        for (auto& [_, value] : aggregated_values.get_values()) {
            fill_metric(mtf, value.m, value.labels, ctx);
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
     *
     * The label matchers the filter uses are stored in \c key.
     */
    std::function<bool(const mi::labels_type&)> make_filter(const http::request& req, details::filter_key& key) {
        // Sorted, so the key doesn't depend on the parameters' order
        std::map<sstring, sstring> expressions;
        auto labels = mi::get_local_impl()->get_labels();
        for (auto&& qp : req.get_query_params()) {
            if (labels.find(qp.first) != labels.end()) {
                expressions.emplace(qp.first, qp.second.back());
            }
        }
        std::unordered_map<sstring, std::regex> matcher;
        for (auto&& [label, expr] : expressions) {
            matcher.emplace(label, std::regex(expr.c_str()));
            key.labels.emplace_back(label, expr);
        }
        return (matcher.empty()) ? _true_function : [matcher](const mi::labels_type& labels) {
            for (auto&& m : matcher) {
                auto l = labels.find(m.first);
                if (!std::regex_match((l == labels.end())? "" : l->second.value().c_str(), m.second)) {
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
        // Build name filters from all __name__ query parameters
        std::vector<details::name_filter> name_filters;
        for (auto name : req->get_query_param_array("__name__")) {
            if (!name.empty()) {
                bool is_prefix = trim_asterisk(name);
                name_filters.push_back({std::move(name), is_prefix});
            }
        }
        // The cache key identifies the filters
        details::filter_key key{.names = name_filters};
        auto filter = make_filter(*req, key);
        write_body_args args{
            .filter = std::move(filter),
            .family_filter = details::make_family_filter(std::move(name_filters), _ctx.prefix),
            .use_protobuf_format = _ctx.allow_protobuf && is_accept_protobuf(req->get_header("Accept")),
            .show_help = req->get_query_param("__help__") != "false",
            .enable_aggregation = req->get_query_param("__aggregate__") != "false",
            .cache_key = std::move(key),
        };
        rep->write_body(args.use_protobuf_format ? "proto" : "txt", [this, args = std::move(args)](output_stream<char>&& s) {
            return write_body(std::move(args), std::move(s));
        });
        return make_ready_future<std::unique_ptr<http::reply>>(std::move(rep));
    }

private:

    future<> write_body(write_body_args args, output_stream<char>&& out_stream) {
        auto s = std::move(out_stream);
        auto families = co_await get_map_value();
        bool use_protobuf = args.use_protobuf_format;

        write_context context{
            .out = s,
            .ctx = _ctx,
            .families = families,
            .m = metric_family_range(families),
            .args = std::move(args)
        };

        co_return co_await (use_protobuf ? context.write_protobuf_representation() : context.write_text_representation())
                .finally([&s] { return s.close(); });
    }

    friend details::test_access;
};

future<> details::test_access::write_body(config cfg, write_body_args args, output_stream<char>&& s) {
    metrics_handler handler(std::move(cfg));
    co_return co_await handler.write_body(std::move(args), std::move(s));
}

details::text_cache_stats details::test_access::cache_stats() {
    auto& cache = local_template_cache();
    auto stats = cache.stats;
    stats.entries = cache.size();
    return stats;
}

void details::test_access::clear_cache() {
    auto& cache = local_template_cache();
    cache.clear();
    cache.stats = {};
}

const void* details::test_access::cached_template() {
    return local_template_cache().any_template();
}

void details::test_access::set_numa_node_mapping(std::optional<std::vector<unsigned>> mapping) {
    numa_node_mapping_for_tests = std::move(mapping);
}

void details::test_access::set_cache_ttl(std::chrono::milliseconds ttl) {
    template_ttl.store(std::chrono::duration_cast<lowres_clock::duration>(ttl).count(), std::memory_order_relaxed);
}

std::function<bool(const mi::labels_type&)> metrics_handler::_true_function = [](const mi::labels_type&) {
    return true;
};

future<> add_prometheus_routes(httpd::http_server& server, config ctx) {
    server._routes.put(httpd::GET, "/metrics", new metrics_handler(ctx));
    return make_ready_future<>();
}

future<> add_prometheus_routes(sharded<httpd::http_server>& server, config ctx) {
    return server.invoke_on_all([ctx](httpd::http_server& s) {
        return add_prometheus_routes(s, ctx);
    });
}

future<> start(httpd::http_server_control& http_server, config ctx) {
    return add_prometheus_routes(http_server.server(), ctx);
}

}
}
