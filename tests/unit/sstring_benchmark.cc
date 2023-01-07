#include <string>

#include <benchmark/benchmark.h>
#include <seastar/core/sstring.hh>

using namespace seastar;

std::string_view foo = "foo";
std::string bar = "bar";
sstring zed = "zed";
const char* baz = "baz";

static void make_sstring(benchmark::State& state) {
    benchmark::ClobberMemory();
    for(auto _ : state) {
        auto s = make_sstring(foo, bar, zed, baz, "bah");
        benchmark::DoNotOptimize(s.data());
    }
}


BENCHMARK(make_sstring);

BENCHMARK_MAIN();
