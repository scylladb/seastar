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

#include <ranges>

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
        return make_ready_future<>();
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

struct metrics_perf_fixture {

    const filter_t always_true = [](auto& mi){ return true; };

    seastar::future<size_t> run_metrics_bench(size_t group_count, size_t families_per_group, size_t series_per_family) {
        using namespace seastar;
        using namespace seastar::metrics;

        remove_existing_metrics();

        metric_groups perf_metrics;

        const size_t series_count = group_count * families_per_group * series_per_family;

        constexpr size_t name_length = 50;

        sstring name_template = (sstring("gauge") + sstring(name_length, '_')).substr(0, name_length - 3) + "{}";
        std::string_view name_template_sv = name_template;

        auto label_template = fmt::runtime("label_value_medium_long_{}");

        label label_0{"some-long-label-name-this-happens-in-real-life"}, label_1{"short"};

        for (auto group_id : irange(group_count)) {
            std::vector<metric_definition> defs;
            for (auto family_id : irange(families_per_group)) {
                auto name = fmt::format(fmt::runtime(name_template_sv), family_id);
                auto desc = fmt::format(fmt::runtime(name_template_sv), family_id);

                for (auto label_id : irange(series_per_family)) {
                    auto label0 = label_0(fmt::format(label_template, label_id));
                    auto label1 = label_1(fmt::format(label_template, label_id));
                    defs.emplace_back(
                        make_counter(name, description(name), {label0, label1}, [] { return 0.0; }));
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
            co_await access{}.write_body(config, false, "", false, true, false, always_true, std::move(out));
        }
        perf_tests::stop_measuring_time();

        co_return series_count * iterations;
    }

};

PERF_TEST_CN(metrics_perf_fixture, test_few_metrics) {
    // only 1 series added, stresses the fixed costs we pay per
    // scrape, regardless of how many metrics we actually have
    co_return co_await run_metrics_bench(1, 1, 1);
}

PERF_TEST_CN(metrics_perf_fixture, test_large_families) {
    // relatively few families, but many series in each, stresses
    // "per series" work
    co_return co_await run_metrics_bench(1, 1, 10000);
}

PERF_TEST_CN(metrics_perf_fixture, test_many_families) {
    // many families, each with only 1 series, stresses "per family"
    // work
    co_return co_await run_metrics_bench(1, 10000, 1);
}

PERF_TEST_CN(metrics_perf_fixture, test_middle_ground) {
    // the goldilocks version
    co_return co_await run_metrics_bench(1, 1000, 10);
}

}
