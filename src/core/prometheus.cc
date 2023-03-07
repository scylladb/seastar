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

#include <seastar/core/prometheus.hh>
#include <sstream>

#include <seastar/core/metrics_api.hh>
#include <seastar/http/function_handlers.hh>
#include <boost/algorithm/string/replace.hpp>
#include <boost/range/algorithm_ext/erase.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/range/algorithm.hpp>
#include <boost/range/combine.hpp>
#include <seastar/core/thread.hh>
#include <seastar/core/loop.hh>
#include <regex>

namespace seastar {

extern seastar::logger seastar_logger;

namespace prometheus {

namespace mi = metrics::impl;

static std::string to_str(seastar::metrics::impl::data_type dt) {
    switch (dt) {
    case seastar::metrics::impl::data_type::GAUGE:
        return "gauge";
    case seastar::metrics::impl::data_type::COUNTER:
    case seastar::metrics::impl::data_type::REAL_COUNTER:
        return "counter";
    case seastar::metrics::impl::data_type::HISTOGRAM:
        return "histogram";
    case seastar::metrics::impl::data_type::SUMMARY:
        return "summary";
    }
    return "untyped";
}

static std::string to_str(const seastar::metrics::impl::metric_value& v) {
    switch (v.type()) {
    case seastar::metrics::impl::data_type::GAUGE:
    case seastar::metrics::impl::data_type::REAL_COUNTER:
        return std::to_string(v.d());
    case seastar::metrics::impl::data_type::COUNTER:
        return std::to_string(v.i());
    case seastar::metrics::impl::data_type::HISTOGRAM:
    case seastar::metrics::impl::data_type::SUMMARY:
        break;
    }
    return ""; // we should never get here but it makes the compiler happy
}

static void add_name(std::ostream& s, const sstring& name, const std::map<sstring, sstring>& labels, const config& ctx) {
    s << name << "{";
    const char* delimiter = "";
    if (ctx.label) {
        s << ctx.label->key()  << "=\"" << ctx.label->value() << '"';
        delimiter = ",";
    }

    if (!labels.empty()) {
        for (auto l : labels) {
            if (!boost::algorithm::starts_with(l.first, "__")) {
                s << delimiter;
                s << l.first  << "=\"" << l.second << '"';
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
    return parallel_for_each(boost::irange(0u, smp::count), [&vec] (auto cpu) {
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
            }
        }
    }

};

void metric_family::foreach_metric(std::function<void(const mi::metric_value&, const mi::metric_info&)>&& f) {
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

std::string get_value_as_string(std::stringstream& s, const mi::metric_value& value) noexcept {
    std::string value_str;
    try {
        value_str = to_str(value);
    } catch (const std::range_error& e) {
        seastar_logger.debug("prometheus: get_value_as_string: {}: {}", s.str(), e.what());
        value_str = "NaN";
    } catch (...) {
        auto ex = std::current_exception();
        // print this error as it's ignored later on by `connection::start_response`
        seastar_logger.error("prometheus: get_value_as_string: {}: {}", s.str(), ex);
        std::rethrow_exception(std::move(ex));
    }
    return value_str;
}

future<> write_text_representation(output_stream<char>& out, const config& ctx, const metric_family_range& m, bool show_help, std::function<bool(const mi::labels_type&)> filter) {
    return seastar::async([&ctx, &out, &m, show_help, filter] () mutable {
        bool found = false;
        std::stringstream s;
        for (metric_family& metric_family : m) {
            auto name = ctx.prefix + "_" + metric_family.name();
            found = false;
            metric_aggregate_by_labels aggregated_values(metric_family.metadata().aggregate_labels);
            bool should_aggregate = !metric_family.metadata().aggregate_labels.empty();
            metric_family.foreach_metric([&s, &out, &ctx, &found, &name, &metric_family, &aggregated_values, should_aggregate, show_help, &filter](auto value, auto value_info) mutable {
                s.clear();
                s.str("");
                if ((value_info.should_skip_when_empty && value.is_empty()) || !filter(value_info.id.labels())) {
                    return;
                }
                if (!found) {
                    if (show_help && metric_family.metadata().d.str() != "") {
                        s << "# HELP " << name << " " <<  metric_family.metadata().d.str() << '\n';
                    }
                    s << "# TYPE " << name << " " << to_str(metric_family.metadata().type) << '\n';
                    found = true;
                }
                if (should_aggregate) {
                    aggregated_values.add(value, value_info.id.labels());
                } else if (value.type() == mi::data_type::SUMMARY) {
                    write_summary(s, ctx, name, value.get_histogram(), value_info.id.labels());
                } else if (value.type() == mi::data_type::HISTOGRAM) {
                    write_histogram(s, ctx, name, value.get_histogram(), value_info.id.labels());
                } else {
                    add_name(s, name, value_info.id.labels(), ctx);
                    s << get_value_as_string(s, value) << '\n';
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
                        s << get_value_as_string(s, h.second) << '\n';
                    }
                    out.write(s.str()).get();
                    thread::maybe_yield();
                }
            }
        }
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
            assert(name.length() >= 3);
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
        sstring metric_family_name = req->get_query_param("__name__");
        bool prefix = trim_asterisk(metric_family_name);
        bool show_help = req->get_query_param("__help__") != "false";
        std::function<bool(const mi::labels_type&)> filter = make_filter(*req);

        rep->write_body("txt", [this, metric_family_name, prefix, show_help, filter] (output_stream<char>&& s) {
            return do_with(metrics_families_per_shard(), output_stream<char>(std::move(s)),
                    [this, prefix, &metric_family_name, show_help, filter] (metrics_families_per_shard& families, output_stream<char>& s) mutable {
                return get_map_value(families).then([&s, &families, this, prefix, &metric_family_name, show_help, filter]() mutable {
                    return do_with(get_range(families, metric_family_name, prefix),
                            [&s, this, show_help, filter](metric_family_range& m) {
                        return write_text_representation(s, _ctx, m, show_help, filter);
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
