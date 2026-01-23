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

#include <seastar/core/iostream.hh>
#include <seastar/core/metrics.hh>
#include <seastar/core/metrics_api.hh>
#include <seastar/core/metrics_registration.hh>
#include <seastar/core/prometheus.hh>
#include <seastar/testing/perf_tests.hh>
#include <seastar/util/tmp_file.hh>

#include <seastar/core/internal/estimated_histogram.hh>

#include <ranges>
#include <stdexcept>

namespace sm = seastar::metrics;

namespace {

auto irange(size_t upper_bound) {
    return std::ranges::views::iota(size_t(0), upper_bound);
}

void remove_existing_metrics() {
    const auto& map = seastar::metrics::impl::get_value_map();

    for (auto& family : map) {
        auto name = family.first;
        for (auto& series: family.second) {
            seastar::metrics::impl::unregister_metric(series.second->get_id());
        }
    }

    assert(seastar::metrics::impl::get_value_map().size() == 0);
}
}

// just records the size of everything written
struct counting_data_sink_impl : public data_sink_impl {
    counting_data_sink_impl(size_t buf_size) : buf_size{buf_size} {}

#if SEASTAR_API_LEVEL >= 9
    future<> put(std::span<temporary_buffer<char>> bufs) override {
        written += std::accumulate(bufs.begin(), bufs.end(), size_t(0), [] (size_t s, const auto& b) { return s + b.size(); });
        return make_ready_future<>();
    }
#else
    virtual future<> put(net::packet data) override {
        abort();
    }

    virtual future<> put(temporary_buffer<char> buf) override {
        written += buf.size();
        return make_ready_future<>();
    }
#endif

    virtual future<> flush() override {
        return make_ready_future<>();
    }

    virtual future<> close() override {
        return make_ready_future<>();
    }

    virtual size_t buffer_size() const noexcept override {
        return buf_size;
    }

    size_t written = 0;
    size_t buf_size;
};

class counting_data_sink : public data_sink {
public:
    counting_data_sink()
        : data_sink(std::make_unique<counting_data_sink_impl>(32000)) {}
};


namespace seastar::prometheus::details {

using data_type = seastar::metrics::impl::data_type;

struct metrics_perf_fixture {

    static constexpr uint64_t histo_min = 1, histo_max = 1000000;
    using histo_type = sm::internal::approximate_exponential_histogram<histo_min, histo_max, 1>;
    const int histo_buckets = histo_type{}.find_bucket_index(-1) + 1;

    const filter_t always_true = [](auto& mi){ return true; };

    template <typename COUNTER_TYPE = double>
    seastar::future<size_t> run_metrics_bench(
        size_t group_count, size_t families_per_group, size_t series_per_family, data_type type, bool enable_aggregation = false, bool use_protobuf = false) {
        using namespace seastar;
        using namespace seastar::metrics;

        remove_existing_metrics();

        metric_groups perf_metrics;

        const size_t series_count = group_count * families_per_group * series_per_family;

        constexpr size_t name_length = 50;

        sstring name_template = (sstring("gauge") + sstring(name_length, '_')).substr(0, name_length - 3) + "{}";
        std::string_view name_template_sv = name_template;

        auto label_template = fmt::runtime("label_value_medium_long_{}");

        label label_0{"some-long-label-name-this-happens-in-real-life"};
        label label_1{"short"};
        label label_2("fixed-label");

        std::optional<histo_type> histogram;
        if (type == data_type::HISTOGRAM) {
            histogram = histo_type{};
            for (double v = histo_min; v < histo_max; v *= 1.3) {
                histogram->add(v);
            }
        }

        for (auto group_id : irange(group_count)) {
            std::vector<metric_definition> defs;
            for (auto family_id : irange(families_per_group)) {
                auto name = fmt::format(fmt::runtime(name_template_sv), family_id);
                auto desc = fmt::format(fmt::runtime(name_template_sv), family_id);

                for (auto label_id : irange(series_per_family)) {
                    auto label0 = label_0(fmt::format(label_template, label_id));
                    auto label1 = label_1(fmt::format(label_template, label_id));
                    auto label2 = label_2("a fixed value");
                    auto l = label0;

                    std::vector<label_instance> labels{label0, label1, label2};

                    auto impl = [&] {
                        if (type == data_type::COUNTER) {
                            return
                                make_counter(name, description(desc), labels, [] { return (COUNTER_TYPE)123.4; });
                        } else if (type == data_type::HISTOGRAM) {
                            return
                                make_histogram(name, description(desc), labels,
                                    [histogram]() { return histogram->to_metrics_histogram(); });
                        }
                        throw std::runtime_error("bad type");
                    }();

                    if (enable_aggregation) {
                       impl.aggregate({label_0, label_1});
                    }

                    defs.emplace_back(std::move(impl));

                }
            }
            perf_metrics.add_group(fmt::format("group-{}", group_id), defs);
        }

        prometheus::config config{};

        using access = prometheus::details::test_access;

        constexpr int iterations = 100;

        perf_tests::start_measuring_time();
        for ([[maybe_unused]] auto _: irange(iterations)) {
            output_stream<char> out{counting_data_sink{}};
            co_await access{}.write_body(config,
                write_body_args{
                    .filter = always_true,
                    .family_filter = [](std::string_view) { return true; },
                    .use_protobuf_format = use_protobuf,
                    .show_help = true,
                    .enable_aggregation = enable_aggregation
                },
                std::move(out));
        }
        perf_tests::stop_measuring_time();

        // if histogram metrics are used there are N buckets per metric, plus 2 for count and sum
        co_return series_count * iterations * (type == data_type::HISTOGRAM ? histo_buckets + 2 : 1);
    }
};

PERF_TEST_CN(metrics_perf_fixture, test_few_metrics) {
    // only 1 series added, but note there are some seastar metrics
    // which are registered too, so this gives a very bloated time as
    co_return co_await run_metrics_bench(1, 1, 1, data_type::COUNTER);
}

PERF_TEST_CN(metrics_perf_fixture, test_large_families) {
    // relatively few families, but many series in each, stresses
    // "per series" work
    co_return co_await run_metrics_bench(1, 1, 10000, data_type::COUNTER);
}

PERF_TEST_CN(metrics_perf_fixture, test_large_families_int) {
    // relatively few families, but many series in each, stresses
    // "per series" work
    co_return co_await run_metrics_bench<size_t>(1, 1, 10000, data_type::COUNTER);
}

PERF_TEST_CN(metrics_perf_fixture, test_large_families_int_aggr) {
    // relatively few families, but many series in each, stresses
    // "per series" work
    co_return co_await run_metrics_bench<size_t>(1, 1, 10000, data_type::COUNTER, true);
}

PERF_TEST_CN(metrics_perf_fixture, test_many_families_int) {
    // many families, each with only 1 series, stresses "per family"
    // work
    co_return co_await run_metrics_bench<size_t>(1, 10000, 1, data_type::COUNTER);
}

PERF_TEST_CN(metrics_perf_fixture, test_many_families_int_aggr) {
    // many families, each with only 1 series, stresses "per family"
    // work
    co_return co_await run_metrics_bench<size_t>(1, 10000, 1, data_type::COUNTER, true);
}

PERF_TEST_CN(metrics_perf_fixture, test_middle_ground) {
    // the goldilocks version
    co_return co_await run_metrics_bench(1, 1000, 10, data_type::COUNTER);
}

PERF_TEST_CN(metrics_perf_fixture, test_middle_ground_int) {
    // the goldilocks version
    co_return co_await run_metrics_bench<size_t>(1, 1000, 10, data_type::COUNTER);
}

PERF_TEST_CN(metrics_perf_fixture, test_middle_ground_protobuf) {
    // the goldilocks version, protobuf output
    // note that protobuf perf is the same with int or float, so we don't both
    // with the int variants
    co_return co_await run_metrics_bench(1, 1000, 10, data_type::COUNTER, false, true);
}

PERF_TEST_CN(metrics_perf_fixture, test_histogram) {
    co_return co_await run_metrics_bench(1, 100, 10, data_type::HISTOGRAM);
}

PERF_TEST_CN(metrics_perf_fixture, test_histogram_protobuf) {
    co_return co_await run_metrics_bench(1, 100, 10, data_type::HISTOGRAM, false, true);
}

PERF_TEST_CN(metrics_perf_fixture, test_histogram_aggr) {
    co_return co_await run_metrics_bench(1, 100, 10, data_type::HISTOGRAM, true);
}

}
