# perf-tests

`perf-tests` is a simple microbenchmarking framework. Its main purpose is to allow monitoring the impact that code changes have on performance.

## Theory of operation

The framework performs each test in several runs. During a run the microbenchmark code is executed in a loop and the average time of an iteration is computed. The `runtime` column shows the median of these per-run averages, followed by their median absolute deviation as a percentage of the median. `iters` is the average number of iterations per run, and `allocs`, `tasks`, `inst` and `cycles` are the median number of memory allocations, tasks executed, instructions retired and CPU cycles per iteration. `overhead` is described in [Overhead column](#overhead-column). The minimum and maximum runtime over all the runs are reported only in the JSON output (`--json-output`).

```
single run iterations:    0
single run duration:      1.000s
number of runs:           5
number of cores:          1
random seed:              1
start/stop overhead:      1.832µs (2.913µs)

test                              iters            runtime     allocs      tasks       inst     cycles   overhead
chain.then_value               28731456    28.75ns ± 1.25%      1.031      1.062     466.93      151.5      0.000
parallel_for_each.suspend_10   37326290    26.54ns ± 0.48%      1.300      1.300     430.61      139.7      0.000
```

`perf-tests` allows limiting the number of iterations or the duration of each run. In the latter case there is an additional dry run used to estimate how many iterations can be run in the specified time. The measured runs are limited by that number of iterations. This means that there is no overhead caused by timers and that each run consists of the same number of iterations.

### Flags

* `-i <n>` or `--iterations <n>` - limits the number of iterations in each run to no more than `n` (0 for unlimited)
* `-d <t>` or `--duration <t>` - limits the duration of each run to no more than `t` seconds (0 for unlimited)
* `--iterations-from-file <file>` - sets the iterations in each run per test, from a JSON object mapping test names to counts, e.g. `{"group.test": 30000}`
* `-r <n>` or `--runs <n>` - the number of runs of each test to execute
* `-t <regex>` or `--test <regex>` - executes only tests whose full name, `<group>.<name>`, matches the regular expression `regex`. The whole name must match, so a group is selected with e.g. `-t 'example\..*'`. Can be given more than once, in which case a test is executed if it matches any of the expressions. If no test matches, a warning is printed and the run fails
* `--list` - lists all available tests
* `--overhead-threshold <ratio>` - warn if the measurement overhead of a test exceeds this fraction of its runtime (default: 0.1, i.e. 10%)
* `--fail-on-high-overhead` - fail the test run if any test exceeds the overhead threshold
* `--no-perf-counters` - do not read the hardware performance counters, so `inst` and `cycles` are reported as 0. This is useful when running the benchmark under perf which will capture its own counters (to avoid multiplexing). This also makes starting and stopping the timers much cheaper (see [Measurement overhead](#measurement-overhead))
* `-S <n>` or `--random-seed <n>` - seeds `seastar::testing::local_random_engine` with `n` plus the shard id on each shard. 0, the default, picks a random seed. The seed used is printed in the output header
* `--parameter <name>=<value>` - sets a test-specific parameter, which tests read with `perf_tests::get_parameter("<name>")`. Can be given more than once. A parameter that is not set reads as an empty string
* `--no-stdout` - do not print the configuration and results to standard output, e.g. when only `--json-output` or `--md-output` is wanted
* `--json-output <file>` - also write the results to `file` as JSON. For each test this includes the `median`, `mad`, `min` and `max` of the runtime per iteration, in nanoseconds
* `--md-output <file>` - also write the results to `file` as a Markdown table, or to standard output if `file` is `-`
* `--columns <names>` - comma-separated list of the columns to include in the text and Markdown output (`iters`, `runtime`, `allocs`, `tasks`, `inst`, `cycles`, `overhead`), or `all` (the default)
* `--mad-columns <names>` - comma-separated list of the columns that also show the median absolute deviation, as a percentage of the median, or `all` (default: `runtime`)

## Example usage

### Simple test

Performance tests are defined in a similar manner to unit tests. Macro `PERF_TEST(test_group, test_case)` allows specifying the name of the test and the group it belongs to. Microbenchmark can either return nothing or a future.

Compiler may attempt to optimise too much of the test logic. A way of preventing this is passing the final result of all computations to a function `perf_tests::do_not_optimize()`. That function should introduce little to none overhead, but forces the compiler to actually compute the value.

```c++
PERF_TEST(example, simple1)
{
    auto v = compute_value();
    perf_tests::do_not_optimize(v);
}

PERF_TEST(example, simple2)
{
    return compute_different_value().then([] (auto v) {
        perf_tests::do_not_optimize(v);
    });
}
```

### Fixtures

As it is in case of unit tests, performance tests may benefit from using a fixture that would set up a proper environment. Such tests should use macro `PERF_TEST_F(test_group, test_case)`. The test itself will be a member function of a class derivative of `test_group`.

The constructor and destructor of a fixture are executed in a context of Seastar thread, but the actual test logic is not. The same instance of a fixture will be used all runs (and iterations) of a given test, but a unique fixture is created for each test. In the example below, exactly 2 fixture objects will be created, for `fixture1` and `fixture2` tests. If you want to share setup _between_ test cases, you can use static members as shown below.

```c++
class example {
protected:
    data_set _ds1;
    data_set _ds2;
private:
    static data_set prepare_data_set();
public:
    example()
        : _ds1(prepare_data_set())
        , _ds2(prepare_data_set())
    { }
};

PERF_TEST_F(example, fixture1)
{
    auto r = do_something_with(_ds1);
    perf_tests::do_not_optimize(r);
}

PERF_TEST_F(example, fixture2)
{
    auto r = do_something_with(_ds1, _ds2);
    perf_tests::do_not_optimize(r);
}
```

### Custom time measurement

Even with fixtures it may be necessary to do some costly initialization during each iteration. Its impact can be reduced by specifying the exact part of the test that should be measured using functions `perf_tests::start_measuring_time()` and `perf_tests::stop_measuring_time()`.

```c++
PERF_TEST(example, custom_time_measurement1)
{
    auto data = prepare_data();
    perf_tests::start_measuring_time();
    do_something(std::move(data));
    perf_tests::stop_measuring_time();
}

PERF_TEST(example, custom_time_measurement2)
{
    auto data = prepare_data();
    perf_tests::start_measuring_time();
    return do_something_else(std::move(data)).finally([] {
        perf_tests::stop_measuring_time();
    });
}
```

#### Measurement overhead

The cost of starting and stopping the timers is substantial, about 1μs for a start/stop pair, due to the overhead of reading the performance counters in the kernel. When using manual time measurement with `start_measuring_time()`/`stop_measuring_time()`, you should ensure that the timed region is substantially longer than this overhead to reduce measurement error. A common approach is to use a loop inside the timed region and return the number of iterations from the test method.

If you do _not_ use these manual methods, the overhead is very low (a few instructions) as the test method is already called in such a loop by the framework, with the time manipulation methods outside that.

##### Overhead column

The framework tracks and reports the estimated measurement overhead in the `overhead` column, as a fraction of the measured runtime (so `0.100` means 10%). This is calculated by:

1. Calibrating the cost of a single `start_measuring_time()`/`stop_measuring_time()` pair at startup
2. Counting how many times these functions are called during each test run
3. Computing `overhead = (call_count × cost_per_call) / measured_runtime` for each run; the column shows the median over the runs

A high overhead (e.g., above 0.1, or 10%) indicates that the timing instrumentation is consuming a significant portion of the measured time, which reduces the accuracy of the results. This typically happens when:
- The timed region is very short (comparable to the ~1μs overhead)
- `start_measuring_time()`/`stop_measuring_time()` are called many times with little work between them

To reduce overhead, either:
- Increase the amount of work in each timed region
- Use an inner loop and return the iteration count, rather than calling start/stop for each iteration

##### Overhead warnings

By default, a warning is printed if any test has median overhead exceeding 10%:

```
WARNING: test 'example.my_test' has high measurement overhead: 15.2% (threshold: 10.0%)
```

You can adjust the threshold with `--overhead-threshold <ratio>` (e.g., `--overhead-threshold 0.2` for 20%), or fail the test run entirely when overhead is too high with `--fail-on-high-overhead`.
