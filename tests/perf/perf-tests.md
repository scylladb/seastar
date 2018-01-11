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

The constructor and destructor of a fixture are executed in a context of Seastar thread, but the actual test logic is not. The same instance of a fixture will be used for multiple iterations of the test.

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

Even with fixtures it may be necessary to do some costly initialisation during each iteration. Its impact can be reduced by specifying the exact part of the test that should be measured using functions `perf_tests::start_measuring_time()` and `perf_tests::stop_measuring_time()`.

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
