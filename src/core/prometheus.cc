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
#include <seastar/http/function_handlers.hh>

#include "prometheus-impl.hh"

#include <boost/algorithm/string/replace.hpp>
#include <boost/algorithm/string.hpp>
#include <seastar/core/thread.hh>
#include <seastar/core/loop.hh>
#include <seastar/util/assert.hh>
#include <algorithm>
#include <cctype>
#include <ranges>
#include <regex>
#include <string_view>
#include <type_traits>

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

/*!
 * \brief An internal label that holds the unit of a metric.
 *
 * Labels starting with a double underscore are never exposed. When a metric
 * carries this label and the OpenMetrics 2.0 format is used, the unit is
 * reported in a `# UNIT` metadata line. The metric name is not changed.
 */
constexpr std::string_view unit_label_name = "__unit";

/*!
 * \brief The Content-Type of an OpenMetrics 2.0 response
 */
constexpr std::string_view openmetrics_2_0_content_type = "application/openmetrics-text; version=2.0.0; charset=utf-8";


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
/*!
 * \brief The bucket layout of a native histogram
 *
 * A native histogram is described by a list of spans and a list of
 * per-bucket (non-cumulative) counts, one count for each nonempty bucket.
 * Each span covers consecutive nonempty buckets. The offset of the first
 * span is the id of its first bucket, the offset of the following spans is
 * the number of empty buckets that separate them from the previous span.
 */
struct native_histogram_layout {
    struct span {
        int32_t offset;
        uint32_t length;
    };
    std::vector<span> spans;
    std::vector<uint64_t> counts;
};

static native_histogram_layout make_native_histogram_layout(const metrics::histogram& h) {
    native_histogram_layout res;
    int32_t id = h.native_histogram.value().min_id;
    uint64_t cumulative = 0;
    int32_t last_bucket_id = 0;
    bool in_span = false;
    for (auto b : h.buckets) {
        // Metrics histograms are cumulative histograms
        // A non empty bucket is bigger than the previous one
        if (cumulative < b.count) {
            // If we are not part of an existing bucket-span, create one
            if (!in_span) {
                res.spans.push_back({id - last_bucket_id, 0});
                in_span = true;
            }
            res.spans.back().length++;
            res.counts.push_back(b.count - cumulative);
        } else if (in_span) {
            // The current bucket is empty, close the existing bucket-span
            in_span = false;
            last_bucket_id = id;
        }
        cumulative = b.count;
        id++;
    }
    return res;
}

static void fill_native_type_histogram(const metrics::histogram& h, ::io::prometheus::client::Histogram* mh) {
    mh->set_sample_count(h.sample_count);
    mh->set_sample_sum(h.sample_sum);
    mh->set_schema(h.native_histogram.value().schema);

    auto layout = make_native_histogram_layout(h);
    for (auto& span : layout.spans) {
        auto bucket_span = mh->add_positive_span();
        bucket_span->set_offset(span.offset);
        bucket_span->set_length(span.length);
    }
    // In the protobuf representation each bucket is stored as a delta from the previous nonempty bucket
    int64_t last_count = 0;
    for (auto count : layout.counts) {
        mh->add_positive_delta(count - last_count);
        last_count = count;
    }
}

static void fill_metric(pm::MetricFamily& mf, const metrics::impl::metric_value& c,
        const metrics::impl::labels_type & id, const config& ctx) {
    switch (c.type()) {
    case seastar::metrics::impl::data_type::GAUGE:
        add_label(mf.add_metric(), id, ctx)->mutable_gauge()->set_value(c.d());
        mf.set_type(pm::MetricType::GAUGE);
        break;
    case seastar::metrics::impl::data_type::SUMMARY: {
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
    case seastar::metrics::impl::data_type::HISTOGRAM:
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
    case seastar::metrics::impl::data_type::REAL_COUNTER:
        [[fallthrough]];
    case seastar::metrics::impl::data_type::COUNTER:
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

// Escapes backslash, double quote and line feed as required by the OpenMetrics text format
static void write_escaped(buf_t& buf, std::string_view str) {
    for (char c : str) {
        switch (c) {
        case '\\': buf.append(R"(\\)"); break;
        case '"': buf.append(R"(\")"); break;
        case '\n': buf.append(R"(\n)"); break;
        default: buf.append(c);
        }
    }
}

// Returns true if name is a valid OpenMetrics label name, [a-zA-Z_][a-zA-Z0-9_]*
static bool is_om_label_name(std::string_view name) {
    return !name.empty() && !std::isdigit(static_cast<unsigned char>(name[0])) &&
            std::ranges::all_of(name, [] (char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; });
}

// Writes a label name, OpenMetrics requires quoting the ones that are not valid label names
static void write_om_label_name(buf_t& buf, std::string_view name) {
    if (is_om_label_name(name)) {
        buf << name;
        return;
    }
    buf << "\"";
    write_escaped(buf, name);
    buf << "\"";
}

[[gnu::always_inline]]
static inline void write_label(buf_t& s, std::string_view key, std::string_view value, bool openmetrics = false) {
    if (openmetrics) {
        write_om_label_name(s, key);
    } else {
        s << key;
    }
    s << "=\"";
    s << value;
    s << "\",";
};

struct no_label {};

template <typename Extra = no_label>
static void write_name_and_labels(buf_t& buf, std::string_view name, auto suffix, const labels_type& labels, const config& ctx, Extra extra = {}, bool openmetrics = false) {

    // extra label can be injected in by the caller, unfortunately prometheus
    // requires labels to be sorted by name, so we have to jump through some
    // hoops to do that.
    constexpr bool has_extra = !std::is_same_v<Extra, no_label>;

    buf << name << suffix << "{";
    if (ctx.label) [[unlikely]] {
        write_label(buf, ctx.label->key(), ctx.label->value(), openmetrics);
    }

    bool wrote_extra = false;

    for (auto& l : labels) {
        std::string_view label_name = l.first;
        if constexpr (has_extra) {
            if (!wrote_extra && extra.name < label_name) {
                write_label(buf, extra.name, extra.value, openmetrics);
                wrote_extra = true;
            }
        }
        if (!is_internal(l.first)) [[likely]] {
            write_label(buf, label_name, l.second.value(), openmetrics);
        }
    }

    if constexpr (has_extra) {
        if (!wrote_extra) {
            write_label(buf, extra.name, extra.value, openmetrics);
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

};

void metric_family::foreach_metric(std::function<void(const mi::metric_value&, const mi::metric_series_metadata&)>&& f) {
    _iterator_state.foreach_metric(std::move(f));
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


template <typename Extra = no_label>
static inline void write_series(buf_t& buf, std::string_view name, const labels_type& labels, const config& ctx, auto v, std::string_view suffix, Extra e = {}) {
    write_name_and_labels(buf, name, suffix, labels, ctx, e);
    static constexpr auto format = std::is_floating_point_v<decltype(v)> ? "{:g}\n" : "{}\n";
    fmt::format_to(buf.back_insert_begin(), FMT_COMPILE(format), v);
};


struct extra_label {
    std::string_view name, value;
};

void write_histogram(buf_t& buf, const config& ctx, std::string_view name, const seastar::metrics::histogram& h, const labels_type& labels) noexcept {

    auto write_one = [&] (auto v, std::string_view suffix) {
        write_series(buf, name, labels, ctx, v, suffix);
    };

    write_one(h.sample_sum, "_sum");
    write_one(h.sample_count, "_count");

    for (auto  i : h.buckets) {
        write_series(buf, name, labels, ctx, i.count, "_bucket",
            extra_label{"le", fmt::format(FMT_COMPILE("{:f}"), i.upper_bound)} );
    }
    write_series(buf, name, labels, ctx, h.sample_count, "_bucket", extra_label{"le", "+Inf"} );
}

void write_summary(buf_t& buf, const config& ctx, std::string_view name, const seastar::metrics::histogram& h, const labels_type& labels) noexcept {

    auto write_one = [&] (auto v, std::string_view suffix) {
        write_series(buf, name, labels, ctx, v, suffix);
    };

    if (h.sample_sum) {
        write_one(h.sample_sum, "_sum");
    }
    if (h.sample_count) {
        write_one(h.sample_count, "_count");
    }

    for (auto  i : h.buckets) {
        write_series(buf, name, labels, ctx, i.count, "",
            extra_label{"quantile", fmt::format(FMT_COMPILE("{:f}"), i.upper_bound)} );
    }
}

void write_value_as_string(buf_t& s, const mi::metric_value& value) noexcept {
    try {
        fmt::format_to(s.back_insert_begin(), FMT_COMPILE("{}\n"), value);
    } catch (const std::range_error& e) {
        seastar_logger.debug("prometheus: write_value_as_string: {}: {}", s.str(), e.what());
        s << "NaN\n";
    } catch (...) {
        auto ex = std::current_exception();
        // print this error as it's ignored later on by `connection::start_response`
        seastar_logger.error("prometheus: write_value_as_string: {}: {}", s.str(), seastar::formattable(ex));
        std::rethrow_exception(std::move(ex));
    }
}

/*
 * OpenMetrics 2.0 text format
 *
 * The OpenMetrics 2.0 format differs from the classic 0.0.4 text format in a few ways:
 * - Complex types (histogram, summary) are written as a single composite value
 *   `{count:..,sum:..,bucket:[..]}` instead of multiple series.
 * - Native histograms have a text representation.
 * - A metric unit is reported with a `# UNIT` line.
 * Metric and label names are reported as is, like in the 0.0.4 format.
 * - The exposition ends with `# EOF`.
 */

// Returns the unit of a metric, taken from the __unit label, or an empty string if it has none.
// A unit is made of metric name characters, a unit with other characters is not reported.
static std::string_view get_unit(const labels_type& labels) {
    auto it = labels.find(sstring(unit_label_name));
    if (it == labels.end()) {
        return {};
    }
    std::string_view unit = it->second.value();
    bool valid = std::ranges::all_of(unit, [] (char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == ':';
    });
    return valid ? unit : std::string_view{};
}

// Writes the # TYPE, # UNIT and # HELP lines of a metric family
static void write_om_metadata(buf_t& buf, std::string_view name, const mi::metric_family_info& info, std::string_view unit, bool show_help) {
    buf << "# TYPE " << name << " " << to_string(info.type) << "\n";
    if (!unit.empty()) {
        buf << "# UNIT " << name << " " << unit << "\n";
    }
    if (show_help && info.d.str() != "") {
        buf << "# HELP " << name << " ";
        write_escaped(buf, info.d.str());
        buf << "\n";
    }
}

// Writes a number in the shortest form that reads back as the same value
template <typename T>
static void write_om_number(buf_t& buf, T v) {
    fmt::format_to(buf.back_insert_begin(), FMT_COMPILE("{}"), v);
}

// Writes a classic (fixed buckets) histogram as an OpenMetrics 2.0 composite value:
// {count:C,sum:S,bucket:[le:count,...,+Inf:C]}
static void write_om_classic_histogram(buf_t& buf, const metrics::histogram& h) {
    buf << "{count:";
    write_om_number(buf, h.sample_count);
    buf << ",sum:";
    write_om_number(buf, h.sample_sum);
    buf << ",bucket:[";
    for (auto b : h.buckets) {
        // the +Inf bucket is optional in the histogram, it is always written last
        if (std::isinf(b.upper_bound) && b.upper_bound > 0) {
            continue;
        }
        // a -Inf bucket can only be the first, as the buckets are in increasing order
        if (std::isinf(b.upper_bound)) {
            buf << "-Inf";
        } else {
            write_om_number(buf, b.upper_bound);
        }
        fmt::format_to(buf.back_insert_begin(), FMT_COMPILE(":{},"), b.count);
    }
    buf << "+Inf:";
    write_om_number(buf, h.sample_count);
    buf << "]}\n";
}

/*!
 * \brief Writes a native histogram as an OpenMetrics 2.0 composite value
 *
 * {count:C,sum:S,schema:N,zero_threshold:0,zero_count:0,positive_spans:[offset:length,...],positive_buckets:[count,...]}
 *
 * Unlike the protobuf representation, the text representation holds the absolute
 * count of each nonempty bucket rather than a delta from the previous bucket.
 * Seastar histograms have no zero bucket and no negative buckets.
 */
static void write_om_native_histogram(buf_t& buf, const metrics::histogram& h) {
    auto layout = make_native_histogram_layout(h);
    buf << "{count:";
    write_om_number(buf, h.sample_count);
    buf << ",sum:";
    write_om_number(buf, h.sample_sum);
    fmt::format_to(buf.back_insert_begin(), FMT_COMPILE(",schema:{},zero_threshold:0,zero_count:0"), h.native_histogram->schema);
    if (!layout.spans.empty()) {
        buf << ",positive_spans:[";
        for (auto& span : layout.spans) {
            fmt::format_to(buf.back_insert_begin(), FMT_COMPILE("{}:{},"), span.offset, span.length);
        }
        buf.pop_back();
        buf << "],positive_buckets:[";
        for (auto count : layout.counts) {
            fmt::format_to(buf.back_insert_begin(), FMT_COMPILE("{},"), count);
        }
        buf.pop_back();
        buf << "]";
    }
    buf << "}\n";
}

static void write_om_histogram(buf_t& buf, const metrics::histogram& h) {
    if (h.native_histogram) {
        write_om_native_histogram(buf, h);
    } else {
        write_om_classic_histogram(buf, h);
    }
}

// Writes a summary as an OpenMetrics 2.0 composite value: {count:C,sum:S,quantile:[q:v,...]}
static void write_om_summary(buf_t& buf, const metrics::histogram& h) {
    buf << "{count:";
    write_om_number(buf, h.sample_count);
    buf << ",sum:";
    write_om_number(buf, h.sample_sum);
    // the quantile list is required, even when it is empty
    buf << ",quantile:[";
    for (auto b : h.buckets) {
        write_om_number(buf, b.upper_bound);
        fmt::format_to(buf.back_insert_begin(), FMT_COMPILE(":{},"), b.count);
    }
    if (buf.back() == ',') {
        buf.pop_back();
    }
    buf << "]}\n";
}

static void write_om_value(buf_t& buf, const mi::metric_value& value) {
    switch (value.type()) {
    case mi::data_type::HISTOGRAM:
        write_om_histogram(buf, value.get_histogram());
        break;
    case mi::data_type::SUMMARY:
        write_om_summary(buf, value.get_histogram());
        break;
    default:
        write_value_as_string(buf, value);
    }
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
    const metric_family_range m;
    const write_body_args args;

    future<> write_text_representation();
    future<> write_openmetrics_representation();
    future<> write_protobuf_representation();
};

future<> write_context::write_text_representation() {
    return seastar::async([this] {
        buf_t s;
        for (metric_family& metric_family : m) {
            if (!args.family_filter(metric_family.name())) {
                continue;
            }
            auto name = ctx.prefix + "_" + metric_family.name();
            bool found = false;
            metric_aggregate_by_labels aggregated_values(metric_family.metadata().aggregate_labels);
            bool should_aggregate = args.enable_aggregation && !metric_family.metadata().aggregate_labels.empty();
            metric_family.foreach_metric([this, &s, &found, &name, &metric_family, &aggregated_values, should_aggregate](const mi::metric_value& value, const mi::metric_series_metadata& value_info) mutable {
                s.clear();
                if ((value_info.should_skip_when_empty() && value.is_empty()) || !args.filter(value_info.labels())) {
                    return;
                }
                if (!found) {
                    if (args.show_help && metric_family.metadata().d.str() != "") {
                        s << "# HELP " << name << " " <<  metric_family.metadata().d.str() << "\n";
                    }
                    s << "# TYPE " << name << " " << to_string(metric_family.metadata().type) << "\n";
                    found = true;
                }
                if (should_aggregate) {
                    aggregated_values.add(value, value_info.labels());
                } else if (value.type() == mi::data_type::SUMMARY) {
                    write_summary(s, ctx, name, value.get_histogram(), value_info.labels());
                } else if (value.type() == mi::data_type::HISTOGRAM) {
                    write_histogram(s, ctx, name, value.get_histogram(), value_info.labels());
                } else {
                    write_name_and_labels(s, name, "", value_info.labels(), ctx);
                    write_value_as_string(s, value);
                }
                out.write(s.data(), s.size()).get();
                thread::maybe_yield();
            });
            if (!aggregated_values.empty()) {
                for (auto&& h : aggregated_values.get_values()) {
                    s.clear();
                    // Labels are already filtered (aggregated labels removed)
                    auto& labels = h.second.labels;
                    auto& value = h.second.m;
                    if (value.type() == mi::data_type::HISTOGRAM) {
                        write_histogram(s, ctx, name, value.get_histogram(), labels);
                    } else {
                        write_name_and_labels(s, name, "", labels, ctx);
                        write_value_as_string(s, value);
                    }
                    out.write(s.data(), s.size()).get();
                    thread::maybe_yield();
                }
            }
        }
    });
}

future<> write_context::write_openmetrics_representation() {
    return seastar::async([this] {
        buf_t s;
        for (metric_family& metric_family : m) {
            if (!args.family_filter(metric_family.name())) {
                continue;
            }
            auto name = ctx.prefix + "_" + metric_family.name();
            bool found = false;
            metric_aggregate_by_labels aggregated_values(metric_family.metadata().aggregate_labels);
            bool should_aggregate = args.enable_aggregation && !metric_family.metadata().aggregate_labels.empty();
            auto write_metadata = [&] (const labels_type& labels) {
                if (found) {
                    return;
                }
                // OpenMetrics 2.0 only recommends suffixing the name with the unit (and counters
                // with _total), it does not require it, so metric names are reported unchanged.
                write_om_metadata(s, name, metric_family.metadata(), get_unit(labels), args.show_help);
                found = true;
            };
            metric_family.foreach_metric([&](const mi::metric_value& value, const mi::metric_series_metadata& value_info) {
                s.clear();
                if ((value_info.should_skip_when_empty() && value.is_empty()) || !args.filter(value_info.labels())) {
                    return;
                }
                write_metadata(value_info.labels());
                if (should_aggregate) {
                    aggregated_values.add(value, value_info.labels());
                } else {
                    write_name_and_labels(s, name, "", value_info.labels(), ctx, no_label{}, true);
                    write_om_value(s, value);
                }
                out.write(s.data(), s.size()).get();
                thread::maybe_yield();
            });
            for (auto&& h : aggregated_values.get_values()) {
                s.clear();
                // Labels are already filtered (aggregated labels removed)
                write_name_and_labels(s, name, "", h.second.labels, ctx, no_label{}, true);
                write_om_value(s, h.second.m);
                out.write(s.data(), s.size()).get();
                thread::maybe_yield();
            }
        }
        out.write("# EOF\n").get();
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
            if ((value_info.should_skip_when_empty() && value.is_empty()) || !args.filter(value_info.labels())) {
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

/*!
 * \brief Returns the quality of the best entry of an Accept header for a media type
 *
 * Media types and parameters are compared in lower case, as they are case-insensitive.
 * match is called with the trimmed ";" separated parameters of each entry of the media type,
 * other than q, and decides whether the entry matches. An entry without q has a quality of 1.
 * Returns 0 when no entry matches, which is also the quality of an entry the client refused (q=0).
 */
static double accept_quality(std::string_view accept, std::string_view media_type, std::function<bool(const std::vector<std::string>&)> match) {
    double best = 0;
    std::vector<std::string> entries;
    boost::split(entries, accept, boost::is_any_of(","));
    for (auto& entry : entries) {
        std::vector<std::string> params;
        boost::split(params, entry, boost::is_any_of(";"));
        for (auto& p : params) {
            boost::trim(p);
            boost::to_lower(p);
            // a parameter value may be a quoted string, version="2.0.0" is version=2.0.0
            if (auto eq = p.find('='); eq != std::string::npos && p.size() >= eq + 3 && p[eq + 1] == '"' && p.back() == '"') {
                p = p.substr(0, eq + 1) + p.substr(eq + 2, p.size() - eq - 3);
            }
        }
        if (params.front() != media_type) {
            continue;
        }
        params.erase(params.begin());
        double q = 1;
        std::erase_if(params, [&q] (const std::string& p) {
            if (!p.starts_with("q=")) {
                return false;
            }
            // a quality value is between 0 and 1
            q = std::clamp(std::strtod(p.c_str() + 2, nullptr), 0.0, 1.0);
            return true;
        });
        if (match(params)) {
            best = std::max(best, q);
        }
    }
    return best;
}

double details::protobuf_accept_quality(std::string_view accept) {
    // the protobuf media type is only meaningful with its parameters
    return accept_quality(accept, "application/vnd.google.protobuf", [] (const std::vector<std::string>& params) {
        return !params.empty();
    });
}

double details::openmetrics_2_0_accept_quality(std::string_view accept) {
    return accept_quality(accept, "application/openmetrics-text", [] (const std::vector<std::string>& params) {
        return std::ranges::find(params, std::string_view("version=2.0.0")) != params.end();
    });
}

double details::text_0_0_4_accept_quality(std::string_view accept) {
    return accept_quality(accept, "text/plain", [] (const std::vector<std::string>& params) {
        return std::ranges::none_of(params, [] (const std::string& p) {
            return p.starts_with("version=") && p != "version=0.0.4";
        });
    });
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
        for (auto&& qp : req.get_query_params()) {
            if (labels.find(qp.first) != labels.end()) {
                matcher.emplace(qp.first, std::regex(qp.second.back().c_str()));
            }
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
        auto accept = req->get_header("Accept");
        double protobuf_quality = _ctx.allow_protobuf ? details::protobuf_accept_quality(accept) : 0;
        double openmetrics_quality = details::openmetrics_2_0_accept_quality(accept);
        // the format the client prefers, protobuf wins a tie
        bool use_protobuf = protobuf_quality > 0 && protobuf_quality >= openmetrics_quality;
        // OpenMetrics 2.0 is only used when preferred over the 0.0.4 text format, which wins a tie
        bool use_openmetrics = !use_protobuf && openmetrics_quality > 0
                && openmetrics_quality > details::text_0_0_4_accept_quality(accept);
        write_body_args args{
            .filter = make_filter(*req),
            .family_filter = details::make_family_filter(std::move(name_filters), _ctx.prefix),
            .use_protobuf_format = use_protobuf,
            .show_help = req->get_query_param("__help__") != "false",
            .enable_aggregation = req->get_query_param("__aggregate__") != "false",
            .format = use_openmetrics ? text_format::openmetrics_2_0 : text_format::prometheus_0_0_4,
        };
        std::string_view content_type = use_protobuf ? "proto" : use_openmetrics ? openmetrics_2_0_content_type : "txt";
        rep->write_body(content_type, [this, args = std::move(args)](output_stream<char>&& s) {
            return write_body(std::move(args), std::move(s));
        });
        return make_ready_future<std::unique_ptr<http::reply>>(std::move(rep));
    }

private:

    future<> write_body(write_body_args args, output_stream<char>&& out_stream) {
        auto s = std::move(out_stream);
        auto families = co_await get_map_value();
        bool use_protobuf = args.use_protobuf_format;
        auto format = args.format;

        write_context context{
            .out = s,
            .ctx = _ctx,
            .m = metric_family_range(families),
            .args = std::move(args)
        };

        auto write = [&] {
            if (use_protobuf) {
                return context.write_protobuf_representation();
            }
            if (format == text_format::openmetrics_2_0) {
                return context.write_openmetrics_representation();
            }
            return context.write_text_representation();
        };
        co_return co_await write().finally([&s] { return s.close(); });
    }

    friend details::test_access;
};

future<> details::test_access::write_body(config cfg, write_body_args args, output_stream<char>&& s) {
    metrics_handler handler(std::move(cfg));
    co_return co_await handler.write_body(std::move(args), std::move(s));
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
