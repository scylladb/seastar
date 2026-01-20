# perf-tests

`perf-tests` is a simple microbenchmarking framework. Its main purpose is to allow monitoring the impact that code changes have on performance.

## Theory of operation

The framework performs each test in several runs. During a run the microbenchmark code is executed in a loop and the average time of an iteration is computed. The shown results are median, median absolute deviation, maximum and minimum value of all the runs.

```
single run iterations:    0
single run duration:      1.000s
number of runs:           5

test                            iterations      median         mad         min         max
combined.one_row                    745336   691.218ns     0.175ns   689.073ns   696.476ns
combined.single_active                7871    85.271us    76.185ns    85.145us   108.316us
```

`perf-tests` allows limiting the number of iterations or the duration of each run. In the latter case there is an additional dry run used to estimate how many iterations can be run in the specified time. The measured runs are limited by that number of iterations. This means that there is no overhead caused by timers and that each run consists of the same number of iterations.

### Flags

* `-i <n>` or `--iterations <n>` – limits the number of iterations in each run to no more than `n` (0 for unlimited)
* `-d <t>` or `--duration <t>` – limits the duration of each run to no more than `t` seconds (0 for unlimited)
* `-r <n>` or `--runs <n>` – the number of runs of each test to execute
* `-t <regexs>` or `--tests <regexs>` – executes only tests which names match any regular expression in a comma-separated list `regexs`
* `--list` – lists all available tests
* `--overhead-threshold <percent>` – warn if measurement overhead exceeds this percentage (default: 10)
* `--fail-on-high-overhead` – fail the test run if any test exceeds the overhead threshold

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
    static data_set perpare_data_set();
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
PERF_TEST(example, custom_time_measurement2)
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

The framework tracks and reports the estimated measurement overhead as a percentage in the `overhead` column. This is calculated by:

1. Calibrating the cost of a single `start_measuring_time()`/`stop_measuring_time()` pair at startup
2. Counting how many times these functions are called during each test run
3. Computing `overhead% = (call_count × cost_per_call) / measured_runtime × 100`

A high overhead percentage (e.g., >10%) indicates that the timing instrumentation is consuming a significant portion of the measured time, which reduces the accuracy of the results. This typically happens when:
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

You can adjust the threshold with `--overhead-threshold <percent>`, or fail the test run entirely when overhead is too high with `--fail-on-high-overhead`.
