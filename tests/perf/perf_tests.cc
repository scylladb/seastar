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
 * Copyright (C) 2018 ScyllaDB Ltd.
 */

#include <algorithm>
#include <boost/program_options/value_semantic.hpp>
#include <cmath>
#include <ranges>
#include <seastar/testing/perf_tests.hh>

#include <cstdio>
#include <fstream>
#include <regex>
#include <type_traits>
#include <utility>
#include <string_view>
#include <array>

#include <fmt/ostream.h>

#include <seastar/core/app-template.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/sharded.hh>
#include <seastar/json/formatter.hh>
#include <seastar/util/later.hh>
#include <seastar/testing/random.hh>
#include <seastar/core/memory.hh>
#include <seastar/core/reactor.hh>

#include <signal.h>

namespace perf_tests::internal {
    struct duration;
}
template <> struct fmt::formatter<perf_tests::internal::duration> : fmt::ostream_formatter {};

namespace perf_tests {
namespace internal {

namespace {

// We need to use signal-based timer instead of seastar ones so that
// tests that do not suspend can be interrupted.
// This causes no overhead though since the timer is used only in a dry run.
class signal_timer {
    std::function<void()> _fn;
    timer_t _timer;
public:
    explicit signal_timer(std::function<void()> fn) : _fn(fn) {
        sigevent se{};
        se.sigev_notify = SIGEV_SIGNAL;
        se.sigev_signo = SIGALRM;
        se.sigev_value.sival_ptr = this;
        auto ret = timer_create(CLOCK_MONOTONIC, &se, &_timer);
        if (ret) {
            throw std::system_error(ret, std::system_category());
        }
    }

    ~signal_timer() {
        timer_delete(_timer);
    }

    void arm(std::chrono::steady_clock::duration dt) {
        time_t sec = std::chrono::duration_cast<std::chrono::seconds>(dt).count();
        auto nsec = std::chrono::duration_cast<std::chrono::nanoseconds>(dt).count();
        nsec -= std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::seconds(sec)).count();

        itimerspec ts{};
        ts.it_value.tv_sec = sec;
        ts.it_value.tv_nsec = nsec;
        auto ret = timer_settime(_timer, 0, &ts, nullptr);
        if (ret) {
            throw std::system_error(ret, std::system_category());
        }
    }

    void cancel() {
        itimerspec ts{};
        auto ret = timer_settime(_timer, 0, &ts, nullptr);
        if (ret) {
            throw std::system_error(ret, std::system_category());
        }
    }
public:
    static void init() {
        struct sigaction sa{};
        sa.sa_sigaction = &signal_timer::signal_handler;
        sa.sa_flags = SA_SIGINFO;
        auto ret = sigaction(SIGALRM, &sa, nullptr);
        if (ret) {
            throw std::system_error(ret, std::system_category());
        }
    }
private:
    static void signal_handler(int, siginfo_t* si, void*) {
        auto t = static_cast<signal_timer*>(si->si_value.sival_ptr);
        t->_fn();
    }
};

}

uint64_t perf_stats::perf_mallocs() {
    return memory::stats().mallocs();
}

uint64_t perf_stats::perf_tasks_processed() {
    return engine().get_sched_stats().tasks_processed;
}

perf_stats perf_stats::snapshot(linux_perf_event* instructions_retired_counter, linux_perf_event* cpu_cycles_retired_counter) {
    return perf_stats(
        perf_mallocs(),
        perf_tasks_processed(),
        instructions_retired_counter ? instructions_retired_counter->read() : 0,
        cpu_cycles_retired_counter ? cpu_cycles_retired_counter->read() : 0
    );
}

time_measurement measure_time;

struct config;
struct result;

struct result_printer {
    virtual ~result_printer() = default;

    virtual void print_configuration(const config&) = 0;
    virtual void print_result(const result&) = 0;

    void update_name_column_length(size_t length) {
        _name_column_length = std::max(1ul, length);
    }
    size_t name_column_length() const {
        return _name_column_length;
    }

private:
    static constexpr size_t DEFAULT_NAME_COLUMN_LENGTH = 40;
    size_t _name_column_length = DEFAULT_NAME_COLUMN_LENGTH;
};

struct config {
    uint64_t single_run_iterations;
    std::chrono::nanoseconds single_run_duration;
    unsigned number_of_runs;
    std::vector<std::unique_ptr<result_printer>> printers;
    unsigned random_seed = 0;
};

// absorbs a single metric across all runs and calculates summary statistics
// on it
template <typename Wrapper = double>
struct float_stats {
    std::vector<double> results_;

    float_stats(size_t runs) {
        results_.reserve(runs);
    }

    // wrap a double value in the Wrapper type, e.g. to distinguish durations
    // from plain doubles
    static Wrapper wrap(double value) {
        return Wrapper{value};
    }

    // add a given value (over all iterations) and a number of iterations
    // the recorded value will be value / iterations
    void add(double value, double iterations) {
        results_.push_back(value / iterations);
    }

    // summary stats
    struct summary {
        double avg, med, min, max, mad;
    };

    summary stats() const {
        auto results = results_;                 // work on a copy
        const size_t count = results.size();
        assert(count);
        const size_t mid = count / 2;
        std::sort(results.begin(), results.end());
        const double median = results[mid];
        std::vector<double> diffs(results.begin(), results.end());
        for (auto& d : diffs) { d = std::fabs(d - median); }
        std::sort(diffs.begin(), diffs.end());
        const double sum = std::accumulate(results.begin(), results.end(), 0.0);
        return { sum / count, median, results.front(), results.back(), diffs[mid] };
    }
};

struct result {
    result(size_t run_count) :
        runtime{run_count},
        allocs{run_count},
        tasks{run_count},
        inst{run_count},
        cycles{run_count}
        {}

    sstring test_name = "";

    uint64_t total_iterations = 0;
    unsigned runs = 0;

    float_stats<duration> runtime;

    float_stats<> allocs;
    float_stats<> tasks;
    float_stats<> inst;
    float_stats<> cycles;
};

struct duration {
    double value;
};

struct scaled_duration {
    double scaled;
    std::string_view unit; // one of ns, us, ms, s
};

struct text_options {
    std::FILE *file = nullptr;
    std::set<sstring> mad_columns;
};

struct format_options {
    size_t width = 0;      // 0 means 'unspecified/disabled'
    int precision = -1;    // -1 triggers adaptive reduction logic
};

// compute the apparent width of a utf-8 string for terminal display.
// The only multi-byte UTF-8 characters we use are ± (U+00B1) and µ (U+00B5),
// both of which are 2 bytes but display as 1 column. All other characters are ASCII.
// Use a concept so we accept either std::string_view directly or any type exposing
// data()/size() (e.g. seastar::sstring) without creating multiple overloads.
size_t apparent_width(std::convertible_to<std::string_view> auto sv) {
    using namespace std::string_view_literals;
    std::string_view view{sv};

    // Count occurrences of ± (UTF-8: 0xC2 0xB1) and µ (UTF-8: 0xC2 0xB5)
    // Each is 2 bytes but displays as 1 column, so subtract 1 for each occurrence
    auto count_subsequence = [](std::string_view haystack, std::string_view needle) {
        size_t count = 0;
        for (auto pos = haystack; !pos.empty(); ) {
            auto found = std::ranges::search(pos, needle);
            if (found.empty()) {
                break;
            }
            ++count;
            pos = std::string_view(found.end(), pos.end());
        }
        return count;
    };

    size_t multibyte_count = 0;
    for (auto pattern : {"\xC2\xB1"sv, "\xC2\xB5"sv}) {
        multibyte_count += count_subsequence(view, pattern);
    }

    return view.size() - multibyte_count;
}

// Definition of adaptive formatter (see forward declaration above).
static sstring format_double_fit(double value, size_t width, size_t default_precision) {
    if (!width || default_precision < 0) {
        return sstring("");
    }

    auto fits = [width](const sstring& s) {
        return apparent_width(s) <= width;
    };

    auto make_hash_fallback = [width]() {
        auto r = uninitialized_string(width);
        for (size_t i = 0; i < width; ++i) {
            r.data()[i] = '#';
        }
        return r;
    };

    if (default_precision > 18) { // more than ~18 digits rarely useful for double
        default_precision = 18;
    }

    // 1 & 2: fixed format with decreasing precision.
    for (int p = default_precision; p >= 0; --p) {
        // Build a runtime format specification for fixed format.
        auto fmt_spec = fmt::format("{{:>{}.{}f}}", width, p);
        auto s = fmt::format(fmt::runtime(fmt_spec), value);
        if (fits(s)) {
            return sstring(s);
        }
    }

    // 3: scientific notation with decreasing precision.
    // Note: scientific adds characters like e+NN which may help for very large/small values.
    for (int p = default_precision; p >= 0; --p) {
        auto fmt_spec = fmt::format("{{:>{}.{}e}}", width, p);
        auto s = fmt::format(fmt::runtime(fmt_spec), value);
        if (fits(s)) {
            return sstring(s);
        }
    }

    // 4: last resort.
    return make_hash_fallback();
}


// Given a value in nanoseconds, scale to the first unit whose value is < 1000.
// Progression: ns -> us -> ms -> s.
static inline scaled_duration calculate_units_and_scale(double nanoseconds) {
    static const std::array units = {
        "ns", "µs", "ms"
    };

    double scaled = nanoseconds;
    for (auto unit : units) {
        if (scaled < 1000.) {
            return {scaled, unit};
        }
        scaled /= 1000.0;
    }
    return {scaled, "s"};
}

inline std::ostream& operator<<(std::ostream& os, duration d) {
    auto sd = calculate_units_and_scale(d.value);
    // fmt hasn't discovered unicode yet so we are stuck with 'us' not 'µs'
    os << fmt::format("{:.3f}{}", sd.scaled, sd.unit);
    return os;
}


// Format a raw duration value expressed in nanoseconds, scaling to the most
// suitable unit (ns, µs, ms, s) and fitting the numeric portion into the
// provided width minus the unit suffix length using the same adaptive logic
// as format_double_fit.
static sstring format_duration_fit(double nanoseconds, size_t width, int precision) {
    auto sd = calculate_units_and_scale(nanoseconds);
    size_t unit_len = apparent_width(sd.unit);
    size_t avail_width = (width > unit_len) ? (width - unit_len) : size_t(1);
    auto num_part = format_double_fit(sd.scaled, avail_width, precision);
    assert(apparent_width(num_part) == avail_width);
    return fmt::format("{}{}", num_part, sd.unit);
}

struct printer {
    format_options fopts{};
    bool include_mad = false;

    sstring operator()(duration t) const {
        return format_duration_fit(t.value, fopts.width, fopts.precision);
    }
    sstring operator()(double d) const {
        return format_double_fit(d, fopts.width, fopts.precision);
    }
    template <typename T>
    sstring operator()(const float_stats<T>& t) const {
        auto st = t.stats();
        sstring ret = (*this)(T{st.med});
        if (include_mad) {
            double rel_med = st.mad / st.med * 100.;
            if (rel_med != rel_med) { rel_med = 0; }
            ret += fmt::format(" \u00B1{:5.2f}%", rel_med);
        }
        return ret;
    }
};

template <typename T>
struct value_traits {
    static constexpr T sample_value{};
};


template <typename T>
struct value_traits<float_stats<T>> {
    static const float_stats<T> sample_value;
};

template<typename T>
const float_stats<T> value_traits<float_stats<T>>::sample_value = [] {
    float_stats<T> f{1};
    f.add(1., 1.);
    return f;
}();


/**
 * A column object encapsulates the logic needed to print one
 * type of result value, usually as a column (or a json attribute).
 *
 * This allows all printers to share a common view of the available
 * columns.
 */
struct column {
    static constexpr int default_width = 9;

    using print_fn = std::function<void(const text_options&, const result&)>;
    using header_fn = std::function<size_t(const text_options&)>;

    template <typename F>
    column(sstring header, int prec, F fn) : header{header}, fopts{default_width, prec} {
        using result_t = std::invoke_result_t<F, const result&>;
        using result_traits = value_traits<result_t>;

        print_text = [=, fopts = fopts](const text_options& opts, const result& r) {
            printer p{fopts, opts.mad_columns.contains(header)};
            fmt::print(opts.file, "{}", p(fn(r)));
        };

        header_size = [=, fopts = fopts](const text_options& opts) {
            printer p{fopts, opts.mad_columns.contains(header)};
            auto v = p(result_traits::sample_value);
            return apparent_width(std::string_view(v));
        };

        to_double = [fn](const result& r) {
            return column::convert_to_double(fn(r));
        };
    }

    static double convert_to_double(duration d) {
        return d.value;
    }

    static double convert_to_double(double d) {
        return d;
    }

    template <typename T>
    static double convert_to_double(float_stats<T> s) {
        return s.stats().med;
    }

    void print_header(text_options topts, const char* str = nullptr) const {
        size_t width = header_size(topts); // calculate header size by actually formatting a representative value
        fmt::print(topts.file, "{:>{}}", str ? str : header, width);
    }

    // column header
    sstring header;

    format_options fopts;

    // used by stdout and md formats to print as text
    print_fn print_text;

    header_fn header_size;

    // used by json format to extract double result
    std::function<double(const result&)> to_double;
};

using columns = std::vector<column>;

static const std::vector<column> common_columns{
    {"allocs", 3, [](const result& r) { return r.allocs;              }},
    {"tasks" , 3, [](const result& r) { return r.tasks;               }},
    {"inst"  , 2, [](const result& r) { return r.inst;                }},
    {"cycles", 1, [](const result& r) { return r.cycles;              }},
};
// json columns
static const std::vector<column> json_columns = [] {
    columns v{
        {"median", 0, [](const result& r) { return duration { r.runtime.stats().med }; }},
        {"mad"   , 0, [](const result& r) { return duration { r.runtime.stats().mad }; }},
        {"min"   , 0, [](const result& r) { return duration { r.runtime.stats().min }; }},
        {"max"   , 0, [](const result& r) { return duration { r.runtime.stats().max }; }},
    };
    v.insert(v.end(), common_columns.begin(), common_columns.end());
    return v;
}();

// text columns
static const columns text_columns = [] {
    columns v{
        {"iters"   , 0, [](const result& r) { return 1. * r.total_iterations / r.runs; }},
        {"runtime" , 2, [](const result& r) { return r.runtime; }}
    };
    v.insert(v.end(), common_columns.begin(), common_columns.end());
    return v;
}();

struct delimiters {
    const char* start_delim = "";
    const char* middle_delim = "  ";
    const char* end_delim = "";
};

struct text_printer : public result_printer {
    text_printer(const columns& columns, const text_options& opts, delimiters delims = {}) :
        columns_{columns},
        opts{opts},
        delims{delims}
    {
        assert(opts.file && delims.start_delim && delims.middle_delim && delims.end_delim);
    }

    void print_header_row(const char* first_col = "test", const char* header_override = nullptr) {
        // start delimeter, and the top-left cell
        fmt::print(opts.file, "{}{:<{}}", delims.start_delim, first_col, name_column_length());
        // the header cells
        for (auto& c : columns_) {
            // middle delimeter
            fmt::print(opts.file, "{}", delims.middle_delim);
            // right align the text in result cells
            c.print_header(opts, header_override);
        }
        // end delimeter
        fmt::print(opts.file, "{}\n", delims.end_delim);
    }

    virtual void print_result(const result& r) override {
        fmt::print(opts.file, "{}{:<{}}", delims.start_delim, r.test_name, name_column_length());
        for (auto& c : columns_) {
            fmt::print(opts.file, "{}", delims.middle_delim);
            c.print_text(opts, r);
        }
        fmt::print(opts.file, "{}\n", delims.end_delim);
    }

    columns columns_;
    text_options opts;
    delimiters delims;
};



struct stdout_printer : text_printer {

    stdout_printer(const columns& columns, text_options options)
        : text_printer(columns, options) {}

    virtual void print_configuration(const config& c) override {
        fmt::print("{:<25} {}\n{:<25} {}\n{:<25} {}\n{:<25} {}\n{:<25} {}\n\n",
                "single run iterations:", c.single_run_iterations,
                "single run duration:", duration { double(c.single_run_duration.count()) },
                "number of runs:", c.number_of_runs,
                "number of cores:", smp::count,
                "random seed:", c.random_seed);

        print_header_row();
    }
};

class json_printer final : public result_printer {
    std::string _output_file;
    std::unordered_map<std::string,
                       std::unordered_map<std::string,
                                          std::unordered_map<std::string, double>>> _root;
public:
    explicit json_printer(const std::string& file) : _output_file(file) { }

    ~json_printer() {
        std::ofstream out(_output_file);
        out << json::formatter::to_json(_root);
    }

    virtual void print_configuration(const config&) override { }

    virtual void print_result(const result& r) override {
        auto& result = _root["results"][r.test_name];
        result["runs"] = r.runs;
        result["total_iterations"] = r.total_iterations;

        for (auto& c : json_columns) {
            result[c.header] = c.to_double(r);
        }
    }
};

struct markdown_printer final : public text_printer {
    constexpr static delimiters md_delims{"| ", " | ", " |"};

    explicit markdown_printer(const columns& columns, text_options options)
        : text_printer(columns, options, md_delims) {}

    ~markdown_printer() {
        if (opts.file != stdout) {
            std::fclose(opts.file);
        }
    }

    void print_configuration(const config&) override {
        // print the header row
        print_header_row("test", nullptr);
        // then the divider row of all -
        print_header_row("-", "-:");
    }
};

static std::vector<pre_run_hook> pre_run_hooks;

int register_pre_run_hook(pre_run_hook hook) {
    pre_run_hooks.emplace_back(std::move(hook));
    return 0; // this return value just helps run this function as a global constructor
}

void performance_test::run_hooks() {
    for (auto& hook : pre_run_hooks) {
        hook(test_group(), test_case());
    }
}

static std::FILE* maybe_open(const sstring& filename) {
    FILE *ret = filename == "-" ? stdout : std::fopen(filename.c_str(), "w");
    if (!ret) {
        throw std::invalid_argument(fmt::format("unable to write to {}", filename));
    }
    return ret;
}

void performance_test::do_run(const config& conf)
{
    _max_single_run_iterations = conf.single_run_iterations;
    if (!_max_single_run_iterations) {
        _max_single_run_iterations = std::numeric_limits<uint64_t>::max();
    }

    signal_timer tmr([this] {
        _max_single_run_iterations.store(0, std::memory_order_relaxed);
    });

    // dry run, estimate the number of iterations
    if (conf.single_run_duration.count()) {
        // switch out of seastar thread
        yield().then([&] {
            tmr.arm(conf.single_run_duration);
            return do_single_run().finally([&] {
                tmr.cancel();
                _max_single_run_iterations = _single_run_iterations;
            });
        }).get();
    }

    result r{conf.number_of_runs};

    uint64_t total_iterations = 0;
    for (auto i = 0u; i < conf.number_of_runs; i++) {
        // switch out of seastar thread
        yield().then([&] {
            _single_run_iterations = 0;
            return do_single_run().then([&] (run_result rr) {
                clock_type::duration dt = rr.duration;
                double ns = std::chrono::duration_cast<std::chrono::nanoseconds>(dt).count();

                auto add = [this](auto& m, double value) {
                    m.add(value, _single_run_iterations);
                };

                add(r.runtime, ns);

                total_iterations += _single_run_iterations;

                add(r.allocs, rr.stats.allocations);
                add(r.tasks, rr.stats.tasks_executed);
                add(r.inst, rr.stats.instructions_retired);
                add(r.cycles, rr.stats.cpu_cycles_retired);
            });
        }).get();
    }

    r.test_name = name();
    r.total_iterations = total_iterations;
    r.runs = conf.number_of_runs;

    for (auto& rp : conf.printers) {
        rp->print_result(r);
    }
}

void performance_test::run(const config& conf)
{
    set_up();
    try {
        do_run(conf);
    } catch (...) {
        tear_down();
        throw;
    }
    tear_down();
}

std::vector<std::unique_ptr<performance_test>>& all_tests()
{
    static std::vector<std::unique_ptr<performance_test>> tests;
    return tests;
}

void performance_test::register_test(std::unique_ptr<performance_test> test)
{
    all_tests().emplace_back(std::move(test));
}

void run_all(const std::vector<std::string>& test_patterns, config& conf) {
    std::vector<std::regex> regexes;
    regexes.reserve(test_patterns.size());
    for (auto& pat : test_patterns) {
        regexes.emplace_back(pat);
    }
    auto match = [&regexes](const performance_test* t) {
        if (regexes.empty()) { return true; }
        for (auto& rgx : regexes) {
            if (std::regex_match(t->name(), rgx)) { return true; }
        }
        return false;
    };
    size_t max_name_column_length = 0;
    for (auto& t : all_tests()) {
        if (match(t.get())) {
            max_name_column_length = std::max(max_name_column_length, t->name().size());
        }
    }
    for (auto& rp : conf.printers) {
        rp->update_name_column_length(max_name_column_length);
        rp->print_configuration(conf);
    }
    for (auto& t : all_tests()) {
        if (match(t.get())) { t->run(conf); }
    }
}

}
}

int main(int ac, char** av)
{
    using namespace perf_tests::internal;
    namespace bpo = boost::program_options;

    app_template app;
    app.add_options()
        ("iterations,i", bpo::value<size_t>()->default_value(0),
            "number of iterations in a single run")
        ("duration,d", bpo::value<double>()->default_value(1),
            "duration of a single run in seconds")
        ("runs,r", bpo::value<size_t>()->default_value(5), "number of runs")
        ("test,t", bpo::value<std::vector<std::string>>(), "tests to execute")
        ("random-seed,S", bpo::value<unsigned>()->default_value(0),
            "random number generator seed")
        ("no-stdout", "do not print to stdout")
        ("json-output", bpo::value<std::string>(), "output json file")
        ("md-output", bpo::value<std::string>(), "output markdown file")
        ("mad-columns", bpo::value<std::string>()->default_value("runtime"),
            "Either 'all' or comma-separated list of columns for which to show MAD as a percentage of median")
        ("columns", bpo::value<std::string>()->default_value("all"),
            "comma separated list of column (by name) to include in text/md output, or 'all'")
        ("list", "list available tests")
        ;

    return app.run(ac, av, [&] {
        return async([&] {
            signal_timer::init();

            config conf;
            conf.single_run_iterations = app.configuration()["iterations"].as<size_t>();
            auto dur = std::chrono::duration<double>(app.configuration()["duration"].as<double>());
            conf.single_run_duration = std::chrono::duration_cast<std::chrono::nanoseconds>(dur);
            conf.number_of_runs = app.configuration()["runs"].as<size_t>();
            conf.random_seed = app.configuration()["random-seed"].as<unsigned>();

            std::vector<std::string> tests_to_run;
            if (app.configuration().count("test")) {
                tests_to_run = app.configuration()["test"].as<std::vector<std::string>>();
            }

            if (app.configuration().count("list")) {
                fmt::print("available tests:\n");
                for (auto&& t : all_tests()) {
                    fmt::print("\t{}\n", t->name());
                }
                return;
            }

            // local helper to split comma-separated strings
            auto split = [](const std::string& str) {
                auto view = str | std::views::split(',') | std::views::transform([](const auto& sub) {
                    return std::string(sub.begin(), sub.end());
                });

                return std::vector<std::string>{view.begin(), view.end()};
            };

            // calculate the effective column set
            auto selected_cols_str = split(app.configuration()["columns"].as<std::string>());
            std::set<std::string> selected_set(selected_cols_str.begin(), selected_cols_str.end());

            columns selected_columns;
            for (const column& col : text_columns) {
                if (selected_set.contains("all") || selected_set.contains(col.header)) {
                    selected_columns.emplace_back(col);
                }
            }

            auto selected_mad_str = split(app.configuration()["mad-columns"].as<std::string>());
            if (std::ranges::find(selected_mad_str, "all") != selected_mad_str.end()) {
                selected_mad_str.clear();
                // add all column names
                for (const column& col : selected_columns) {
                    selected_mad_str.emplace_back(col.header);
                }
            }

            text_options base_topts{
                .file = nullptr,
                .mad_columns{selected_mad_str.begin(), selected_mad_str.end()},
            };

            if (!app.configuration().count("no-stdout")) {
                auto topts = base_topts;
                topts.file = stdout;
                conf.printers.emplace_back(std::make_unique<stdout_printer>(selected_columns, topts));
            }

            if (app.configuration().count("json-output")) {
                conf.printers.emplace_back(std::make_unique<json_printer>(
                    app.configuration()["json-output"].as<std::string>()
                ));
            }

            if (app.configuration().count("md-output")) {
                auto topts = base_topts;
                topts.file = maybe_open(app.configuration()["md-output"].as<std::string>());
                conf.printers.emplace_back(
                    std::make_unique<markdown_printer>(selected_columns, topts
                ));
            }

            if (!conf.random_seed) {
                conf.random_seed = std::random_device()();
            }
            smp::invoke_on_all([seed = conf.random_seed] {
                auto local_seed = seed + this_shard_id();
                testing::local_random_engine.seed(local_seed);
            }).get();

            run_all(tests_to_run, conf);
        });
    });
}
