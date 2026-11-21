# Testing {#sol:testing}

## Exercise 1

The patch, `examples/solutions/shoal-blpop-test.patch`, adds a test to
Shoal 16's `unit_tests.cc`, which uses the fixture of the chapter, whose
`run()` executes a request with `execute()`, as a connection would:

```{.cpp include="examples/solutions/shoal-blpop-test.patch" file="unit_tests.cc" snippet="blpop-test"}
```

`run()` returns a future, which the test keeps, rather than waits for:
the `BLPOP` waits, and the test goes on, in the same fiber, to send the
`LPUSH`. But not at once. If the `LPUSH` came first, the `BLPOP` would
find the list's element when it started, and return it without waiting,
and the test would pass without testing what it's meant to, the waking
up of a waiting client. So the test waits until the `BLPOP` is waiting:
until the store counts a blocked client, which `BLPOP` does only after
its wait is in the key's line of waiting clients. A test can't sleep for
a while instead, and hope: a `debug` build, or a busy machine, may take
longer than any while we'd choose. Checking the counter after each
`yield()`, which lets the `BLPOP`'s fiber run, waits exactly as long as
needed.

The test also checks, before the `LPUSH`, that the `BLPOP`'s future isn't
ready, and after it, that the reply names the key and the element. It
passes in both `release` and `debug` builds.

## Exercise 2

The program, `examples/solutions/testing-ex2.cc`, has two fibers: one
prepares a greeting, after a `yield()`, which stands for anything that
makes it wait, and the other checks it, after its own `yield()`:

```{.cpp include="examples/solutions/testing-ex2.cc" snippet="wrong"}
```

The second fiber assumes that the first is done by then: the first
yielded first, so its continuation is first in the queue, and in a
`release` build, which runs a queue's tasks in order, it runs first. But
Seastar doesn't promise that, and the `debug` build, which shuffles the
queue, as the chapter explained, often runs the second fiber's
continuation first. The program tries it 100 times; here it is built
against a `release` Seastar, and against a `debug` one (as `Debug`, as
the chapter explained):

```
$ ./testing-ex2-release -c1
wrong: no greeting in 0 of 100
right: no greeting in 0 of 100
$ ./testing-ex2-debug -c1
wrong: no greeting in 51 of 100
right: no greeting in 0 of 100
```

Three runs of the `debug` build missed it in 51, 51 and 50 of the 100
tries, about half, as a coin flip would. The correct version doesn't
guess when the greeting is ready. It waits for it, with a future that the
first fiber makes ready when it's done:

```{.cpp include="examples/solutions/testing-ex2.cc" snippet="right"}
```

A fiber that needs another's result must wait for it, with a future, a
condition variable or a semaphore ([chapter](#ch:coordination)), and
never for its turn, which no build promises.

## Exercise 3

The fuzz test, `examples/solutions/table-fuzz.cc`, reads the input as a
sequence of operations of three bytes each, and applies each to a
`key_table` and to a `std::unordered_map`, the simpler structure whose
results we trust:

```{.cpp include="examples/solutions/table-fuzz.cc" snippet="fuzz"}
```

Any difference calls `abort()`, which libFuzzer reports as a crash, with
the input that caused it, saved to a file, so that we can run it again.
The number of keys matters. Our first version used 64 keys, so that
operations would often meet, but the table never grew beyond a few small
tables, and never split one. With 8,192 keys, and inputs long enough to
insert thousands, it does, which we confirmed with a breakpoint, in gdb,
on `key_table::split()`. Every 1,024 operations, a scan of the whole
table, with `scan()`, which Shoal's `KEYS` and `SCAN` use, must find
exactly the map's keys and values.

The test doesn't need a reactor, nor most of Seastar: `table.hh` uses
only `sstring`, from Seastar's headers, which needs two functions from
the library, which throw its errors. Linking the library would bring
Seastar's allocator, which replaces `malloc()`, and conflicts with
AddressSanitizer's, so the test defines the two functions itself. We
compiled it with Clang, and ran it for a minute, with inputs of up to
60,000 bytes from the start, rather than libFuzzer's default of short
inputs that it lengthens slowly:

```
$ clang++ -std=c++23 -O1 -g -fsanitize=fuzzer,address,undefined \
      -DSHOAL_FUZZER $(pkg-config --cflags seastar) table-fuzz.cc -o table-fuzz
$ mkdir corpus
$ ./table-fuzz -max_total_time=60 -max_len=60000 -len_control=0 corpus
...
#96524  DONE   cov: 478 ft: 3105 corp: 185/556Kb lim: 60000 exec/s: 1582 rss: 463Mb
```

It ran the function 96,524 times, and found no difference: `cov` is the
number of branches in the code that the inputs took, and `corp` the
inputs that it kept, because each reached something new. Built without
`SHOAL_FUZZER`, with any compiler, the file has its own `main()`, which
calls the same function with 200 random inputs: a much weaker test,
which checks that the test itself works, and which `check_examples`
runs.

## Exercise 4

The patch, `examples/solutions/shoal-execute-perf.patch`, adds a
benchmark to Shoal 16's `perf_tests.cc`:

```{.cpp include="examples/solutions/shoal-execute-perf.patch" file="perf_tests.cc" snippet="perf-execute"}
```

The fixture starts the store, which, unlike the chapter's table, must be
started, and stopped, with a future: `perf_tests` constructs a fixture in
a Seastar thread, so its constructor can wait, with `get()`. With `-c1`,
the only shard is the key's. And the test drops the reply, a
`future<sstring>`, whose value `perf_tests` would otherwise take for the
number of things that the call did.

```
$ build/shoal_perf_tests -c1 -m1G --blocked-reactor-notify-ms 2000
single run iterations:    0
single run duration:      1.000s
number of runs:           5
number of cores:          1
random seed:              1395606604
start/stop overhead:      1.118µs (1.510µs)

test                           iters            runtime     allocs      tasks       inst     cycles   overhead
resp.parse_pipeline         24039000    41.65ns ± 0.33%      2.120      0.000    1046.34      193.1      0.000
table_fixture.find          13616000    73.30ns ± 0.16%      0.000      0.000     158.81      340.7      0.000
store_fixture.execute_get   24346084    41.89ns ± 0.86%      0.000      0.000     904.00      194.5      0.000
```

Neither takes longer: parsing a `GET` and executing it each take about
42 ns, 41.0 and 41.8 ns in the best of three runs, and about 190 cycles
each. They do different work in that time. Parsing runs more
instructions, 1,046 against 904, and allocates about twice for each
request; executing allocates nothing, since the reply, `$5\r\nvalue\r\n`,
is short enough for `sstring` to keep inside itself, and its key is
always in the CPU's cache, unlike the table test's.

Together, they're about 84 ns. In [chapter](#ch:memory), one shard of
Shoal 12 served 4.56 million `GET`s of one key a second, about 220 ns
each, with pipelines of 64 requests. So parsing and executing are less
than half of what a request costs; the rest goes to everything else that
a request needs: reading it from the socket, the connection's loop,
and writing the reply.

## Exercise 5

The patch, `examples/solutions/shoal-large-rewrite.patch`, changes no
code. It adds a test:

```{.python include="examples/solutions/shoal-large-rewrite.patch" file="test_solutions.py" snippet="test"}
```

The rewrite writes its new log early when more than a megabyte of it
waits to be written, on one shard, so the test writes 20,000 values of a
kilobyte, about 10 MB on each of the two shards that the tests run. It
then checks what any rewrite test checks: that a restart after the
rewrite finds every value. It passes against Shoal and against Valkey.
With the coverage build of the chapter, and the test added to the run
with `--test`:

```
$ (cd ../test && ./run 16 --shoal ../v16/build-coverage/shoal \
       --test test_solutions.py -k large_rewrite)
$ llvm-profdata merge -o shoal.profdata coverage/*.profraw
$ llvm-cov show build-coverage/shoal -instr-profile shoal.profdata \
      $PWD/commands.cc | grep "rewriting->write"
 1531|      1|                co_await rewriting->write();
```

The line ran: with the chapter's options, the coverage build tells only
whether each line ran, 1 or 0, and without the new test it was 0. So
did `append_only_log::write()`, which it calls, and which, until this
test, no test had run at all.

