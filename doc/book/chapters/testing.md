# Testing {#ch:testing}

A Seastar application is tested as any program is, from the outside
and from the inside, but much of what makes its tests good is particular
to Seastar: how to run a Seastar server for tests, Seastar's own testing
library, which runs tests inside a reactor, the `debug` build, which
changes how the reactor runs tasks, to expose bugs that the other builds
hide, and Seastar's library for benchmarks. This chapter explains them,
with Shoal as the example. It starts with *integration tests*, which
test a whole server from the outside, and *unit tests*, which test one
part of a program from the inside, in a Seastar program of their own,
with Seastar's testing library: its three kinds of test case, and its
fixtures. Then it explains the `debug` build, in which tests should also
run, and what it does to find the bugs that a `release` build hides,
measures how much of a program's code its tests run, and ends with
fuzzing and benchmarking. Shoal's version 16 adds unit tests, and the
fix of a bug that the `debug` build found.

## Integration tests {index="testing!integration tests"}

An integration test starts the server, talks to it with a client, as
its users would, and checks the replies. Since it's outside the server,
it can be written in any language. ScyllaDB's are mostly in Python, with
`pytest`, and so are Shoal's, which have come with every version since
the first: `shoal/test/` has a file of tests for each version, which use
the `redis` client library, and a script, `run`, which starts a Shoal,
runs the tests of its version and of all the versions before it, and
stops it. A few things make such tests better, and some are particular
to Seastar servers.

**Test the tests.** If there's another server with the same protocol,
run the tests against it too. `./run 16 --valkey` runs Shoal's tests
against a real Valkey server. A test that fails against Valkey tests the
wrong thing, or tests Shoal's quirks rather than Redis's behavior; a
test that passes against both tests what Shoal promises. Tests of what
Valkey doesn't have, such as Shoal's admin API, skip themselves when
they find Valkey. This has caught mistakes in our tests more than once.
ScyllaDB does the same: its CQL tests can run against Cassandra, and the
tests of its DynamoDB-compatible API against Amazon's DynamoDB.

**Share a server, and start another when needed.** Most of Shoal's tests
share the server that `run` started, and use keys of their own, so that
they don't see each other's. A test that stops the server, restarts it,
or needs other options, such as a replica, starts servers of its own,
with `start_server()` of `conftest.py`, which picks a free port, gives
each server its own directory, and waits until the server accepts
connections. [Chapter](#ch:rpc) told what that waiting must allow for: a
killed Seastar server's port may go on accepting connections for a
moment. When the test ends, servers still running are stopped as a user
would stop them, with `SIGTERM`, and are killed only if they don't stop
in 10 seconds: a server that's killed doesn't do what it does as it
exits, such as writing its code coverage, as a section below shows.

**The server runs as tests need.** `run` starts Shoal with
`-c2 -m1G --overprovisioned`, as ScyllaDB's test framework starts its
servers with `--smp 2 -m 1G --overprovisioned`. Two shards are the
fewest that find the bugs of talking between shards, and a gigabyte is
enough. `--overprovisioned`, which
[chapter](#ch:how-seastar-runs-your-code) introduced, is
`--idle-poll-time-us 0 --thread-affinity 0 --poll-aio 0`: the shards
don't poll when they have nothing to do, so that several servers and
their clients can share a machine's cores, as tests make them; the
shards' threads aren't pinned to cores, so that two servers' shards
don't fight over the same ones; and disk I/O isn't polled either. It
also lets `-c` ask for more shards than the machine has cores, which a
small machine or container for continuous integration may need: without
it, a test that asks for two shards where there is one core doesn't
start, with "insufficient processing units".
[Chapter](#ch:observability) measured what idle polling costs a machine,
and what turning it off costs a server.

## Unit tests {index="testing!unit tests"}

A unit test tests a piece of a program, such as Shoal's parser, by
calling it, in a program of its own, which is faster than a server and a
client, and can try what a client can't easily make happen. Seastar's
testing library, `seastar_testing`, which Seastar installs next to
Seastar itself, runs tests written with Boost.Test, the testing
framework of the Boost libraries, in a Seastar program. Shoal's version
16 builds one, `shoal_unit_tests`, from `unit_tests.cc` and the Shoal
sources that it tests:

```{.cmake include="shoal/v16/CMakeLists.txt" snippet="unit-tests"}
```

`unit_tests.cc` defines `SEASTAR_TESTING_MAIN` before it includes
Seastar's testing headers, which then give the program its `main()`.
That `main()` runs Boost.Test, in the program's main thread, and starts
a Seastar application, on a thread of its own, which runs each test
case that needs Seastar, one at a time, on shard 0. Boost.Test's own
options come before a `--` on the command line, and Seastar's after it:

```
$ build/shoal_unit_tests -- -c2 -m1G
Running 5 test cases...
build_id=23856dfc81d6e5305d482a58ea87f3eda46be193
random-seed=3711795196

*** No errors detected
```

There are three kinds of test case, in one program.
[`SEASTAR_TEST_CASE`]{index="`SEASTAR_TEST_CASE`" .main}, from
`<seastar/testing/test_case.hh>`, is a test whose body returns a future,
usually a coroutine, which runs in the reactor. Shoal's parser is given
a request cut in two at every possible place, and must find the same
requests every time:

```{.cpp include="shoal/v16/unit_tests.cc" snippet="reader-test"}
```

[`util::as_input_stream()`]{index="`seastar::util::as_input_stream()`" .main}, from `<seastar/util/memory-data-source.hh>`,
makes an input stream of the buffers that it's given, which is how a
test feeds a parser what would otherwise come from a socket. Our first
version of this test failed, at the first cut, and not because of the
parser: an empty buffer, which a cut at either end makes, is the end of
the stream, so the parser saw no input at all.

[`SEASTAR_THREAD_TEST_CASE`]{index="`SEASTAR_THREAD_TEST_CASE`" .main},
from `<seastar/testing/thread_test_case.hh>`, runs its body in a
Seastar thread ([chapter](#ch:threads)), where `get()` waits for a
future, which suits a test that reads as a list of steps:

```{.cpp include="shoal/v16/unit_tests.cc" snippet="thread-test"}
```

`BOOST_REQUIRE_EXCEPTION` checks that an expression throws an exception
of a type, which a predicate also accepts; Seastar's
`testing::exception_predicate::message_contains()` and
`message_equals()`, from `<seastar/testing/seastar_test.hh>`, make the
predicates that check its message. And a test
that needs no reactor at all is a plain `BOOST_AUTO_TEST_CASE`, which the
same program runs, in its main thread:

```{.cpp include="shoal/v16/unit_tests.cc" snippet="boost-test"}
```

The test cases share one Seastar application, so what one leaves on a
shard, such as a `thread_local` variable that it set, or a background
fiber that it didn't wait for, the next one finds. The runner checks one
thing at the end: if any failed future was abandoned, without anyone
looking at its exception, the program fails at the end, with exit code
3. It also prints the seed of the random-number engine that it gives
each shard, `testing::local_random_engine`, from
`<seastar/testing/random.hh>`: a test that draws its random numbers from
it can be run again with the same numbers, with `--random-seed`.

### Fixtures {index="testing!fixtures"}

Many tests need the same thing set up before them, and taken down after
them, even when they fail: a *fixture*. Boost.Test's fixtures can't
wait, so Seastar has its own:
[`SEASTAR_FIXTURE_TEST_CASE`]{index="`SEASTAR_FIXTURE_TEST_CASE`" .main},
from `<seastar/testing/test_fixture.hh>`, added in 2025. The fixture is
a class whose `setup()` and `teardown()` may return futures; the test's
body is a member function of a class derived from the fixture, so it
uses the fixture's members as its own. Shoal's fixture starts the
store on every shard:

```{.cpp include="shoal/v16/unit_tests.cc" snippet="fixture"}
```

Test cases run on shard 0, so a test reaches the other shards as the
program does, through `sharded<>` services and `invoke_on()`: here,
`execute()` forwards each request to its key's shard, so these tests
cross shards, as long as they run with `-c2` or more, as above.
The test's body is a `const` member function, as the macro declares it,
so the members that it changes must be `mutable`. A fixture can also be
shared by the tests of a test suite, with
`BOOST_AUTO_TEST_SUITE(name, *testing::async_fixture<F>())`, which sets
it up once before the suite's first test, and takes it down after the
last.

### Files, time and failures {index="testing!failure injection"}

Three more of Seastar's tools for tests are worth knowing, though
Shoal's unit tests don't need them.

[`tmp_dir`]{index="`seastar::tmp_dir`" .main}, from `<seastar/util/tmp_file.hh>`,
makes a new directory for a test's files, under `$TMPDIR`, or `/tmp`,
and removes it, with everything in it, when the test is done, even if
it failed: `tmp_dir::do_with()` calls a function with the `tmp_dir`,
whose `get_path()` is the directory, and `tmp_dir::do_with_thread()`
calls it in a Seastar thread.

`manual_clock`, which [chapter](#ch:time) introduced, is a clock whose
time moves only when a test calls `manual_clock::advance()`, which
schedules the functions of the timers that it made due, on every shard.
A test of something that happens after an hour then takes no time at
all, if the code that it tests lets it choose the clock, as a template
parameter. Shoal's store doesn't: its timers are `timer<expiry_clock>`,
and `expiry_clock` is `lowres_clock`, so Shoal's tests of expiry wait in
real time, with margins.

Code that handles a failure is code too, and is rarely run. As
[chapter](#ch:memory) explained, Seastar's allocator can fail on
purpose: `memory::with_allocation_failures()`, from
`<seastar/util/alloc_failure_injector.hh>`, calls a function again and
again, making its first allocation throw `std::bad_alloc`, then its
second, and so on, until a call has no failure, so that a test can check
that every failure leaves things as they should be. It needs Seastar's
own allocator, which the `debug` build replaces, so the failures are
injected only in the `dev` build, or in a Seastar configured with
`--enable-alloc-failure-injector`. For other failures, Seastar has nothing,
and both of the large Seastar projects wrote their own *error
injection*: named points in the server's code at which a test, through
the server's REST API, makes it throw an exception, sleep, or wait.
ScyllaDB's `utils::error_injection` is compiled in only in its `debug`,
`dev` and `sanitize` builds, and Redpanda's `finjector::honey_badger`
only in builds without `NDEBUG`, so that a `release` build pays nothing
for them.

## The debug build {index="debug build"}

Seastar's `debug` build, which [chapter](#ch:getting-started) compared
with the others, is slower than `release`, 150 times in
[chapter](#ch:futures-and-promises)'s measurement, and is meant to be:
it changes what Seastar does, so that bugs that a `release` build hides
show, and checks what a `release` build trusts. Tests should run in it
too, but not only in it: it's too slow for the many runs of every day,
which look for regressions while a developer works, and which, as
[chapter](#ch:getting-started)'s table of the build modes suggested,
run in the `dev` build, optimized and with every assertion on. The
`debug` build's runs, fewer, catch what those can't. In Seastar's source,
these are what the `debug` mode (and the `sanitize` mode, its optimized
twin) changes:

- **Every `co_await` and every continuation yields.** `need_preempt()`
  always says yes, and a continuation of a future that is already ready,
  which a `release` build runs at once, is scheduled as a task. Code
  that assumes that a future is ready, or that nothing runs between two
  steps, breaks.
- **Tasks run in a shuffled order.** Each new task swaps places with a
  random one in the queue, and the reactor runs a random number of tasks
  between polls, so code that depends on the order of independent tasks
  breaks too.
- **The system's allocator replaces Seastar's.** Memory isn't divided
  among the shards (`-m` has no effect), and AddressSanitizer, which
  finds the use of memory after it's freed, accesses beyond an
  allocation's end and leaks ([chapter](#ch:memory-leaks)), can watch
  every allocation. UndefinedBehaviorSanitizer checks, as the program
  runs, for what C++ leaves undefined, such as a signed integer that
  overflows.
- **Shards are checked.** A `shared_ptr` or `lw_shared_ptr` whose count
  is touched on a shard other than its own, a `foreign_ptr` used off its
  shard, and a promise made ready on the wrong shard are each reported
  ([chapter](#ch:shared-nothing)).
- **Stacks are guarded.** A Seastar thread's stack gets a guard page,
  and the compiler adds stack-clash protection, so an overflow crashes
  at once, rather than overwriting memory. (The `dev` build has these
  too.)

A program built against a `debug` Seastar must be built as `Debug` too,
since Seastar's definitions, such as `SEASTAR_DEBUG`, apply to the
program's own code, which includes Seastar's headers, only in that
configuration. For Shoal, in a build directory of its own, with an
installed `debug` Seastar (as [chapter](#ch:getting-started) configured
Shoal with an installed `release` one), and its tests run against that
build:

```
$ cmake -S . -B build-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_PREFIX_PATH="$HOME/seastar-install-debug;$SEASTAR_SRC/build/debug/_cooking/installed"
$ cmake --build build-debug
$ cd ../test && ./run 16 --shoal ../v16/build-debug/shoal
```

Shoal 16 in a `debug` build passes its unit tests, but the first time we
ran its integration tests in one, nine of them failed. Seven failed
because Shoal crashed when it started with a log to replay, with an
assertion:

```
future.cc:144: void seastar::future_state_base::ignore(): Assertion `0 && "invalid state for ignore"' failed.
```

The replay of the log, since version 10
([chapter](#ch:files-and-storage)), executed each request with
`execute_here()`, and ignored the future that it returned, with
`ignore_ready_future()`, which assumes that the future is ready: with
no log to commit to yet, nothing waits. In a `release` build it was
ready. But `execute_here()` ends with a `handle_exception()`, a
continuation, which a `debug` build schedules instead of running, so
the future wasn't ready yet. That was a bug in every version since 10,
which only the `debug` build showed: the code relied on something that
Seastar doesn't promise. Version 16 replays a request with the command's
own function, which doesn't wait at all:

```{.cpp include="shoal/v16/commands.cc" snippet="replay"}
```

Of the other two failures, one was a mistake in a test.
`test_copy_keys`, from [chapter](#ch:rpc), gives a key 1,000 seconds to
live, starts a replica, and checked that the replica's copy of the key
had more than 990 seconds left. In a `debug` build the copy took more
than 10 seconds. The test now allows 100: a margin in a test must allow
for the slowest build that it runs in.

The last one was a bug in Seastar. `test_many_large_values`, which
[chapter](#ch:coordination) added, sends ten values of 60 MB at once,
600 MB that Shoal writes to its log. In a `debug` build, the test timed
out in about half of the runs, because Shoal wrote its log at about 2.4
MB a second. That wasn't the `debug` build's slowness: in the other runs
of the same program, the test passed. And the slow processes
weren't busy. Their threads were asleep most of the time, while each
write, which the disk finished in about a millisecond, took about 75.

The reactor's loop, as [chapter](#ch:how-seastar-runs-your-code)
explained, calls its pollers one after another, small objects that each
check for one kind of event. Two of them must run in a fixed order. The
I/O scheduler's poller ([chapter](#ch:io-scheduler)) puts the requests
that it decides to send now in a buffer, and the kernel submission
poller, which must come after it, gives the buffer's requests to the
kernel. The reactor creates the two in that order, but it adds each
poller to its list with a task, and the `debug` build shuffles all
tasks, these too. So in a `debug` build each shard gets its pollers in a
random order, and in about half of the processes the shard that wrote
the log had these two the wrong way around. Then a request that the
I/O scheduler put in the buffer waited there until the next pass. With
the io_uring backend, the reactor may sleep while I/O is in flight, and
the I/O scheduler's poller doesn't report that it did anything, so the
reactor often went to sleep first. The request stayed in the buffer
until something else, a timer, woke the reactor up, about 75
milliseconds later. Every write of the log waited that long.

A `release` build runs tasks in order, so it adds the pollers in the
order that it creates them, and has no such bug. The `debug` build
didn't make the bug more likely; it made it possible, in code that
relied on something that the `debug` build deliberately doesn't keep,
the order of tasks. That's twice in one chapter: Shoal's replay relied
on a future being ready, and Seastar's reactor on the order of its
tasks, and both worked in `release` only because of something that they
weren't promised. We reported the bug as Seastar issue
<https://github.com/scylladb/seastar/issues/3844>. Until it's
fixed, the tests of a `debug` build can avoid it with a longer
`--idle-poll-time-us`, which [chapter](#ch:how-seastar-runs-your-code)
introduced: the reactor polls for that long before it sleeps, so a
request in the buffer is sent in the next pass:

```
$ ./run 16 --shoal ../v16/build-debug/shoal --server-option="--idle-poll-time-us 100000"
```

With it, all of Shoal 16's tests pass in a `debug` build, in 116
seconds, against 30 in a `release` build.

## Code coverage {index="testing!code coverage"}

How much of a program do its tests run? A *coverage* build records, as
the program runs, which pieces of its code ran, and a tool then shows
the lines that no test reached. The tools aren't particular
to Seastar. The most precise today is Clang's *source-based* coverage,
which ties what it records to the source rather than to the compiled code, so
that its report follows the source even in an optimized build: the
compiler options
`-fprofile-instr-generate -fcoverage-mapping`, and the tools
`llvm-profdata` and `llvm-cov`. GCC has its own, the option `--coverage`
and the tool `gcov`. Redpanda uses Clang's, through its Bazel build, and
so does ScyllaDB, whose
`test.py --coverage` runs its tests and makes the report. For Shoal,
with a Seastar built by Clang, and with two more options, which the
third point below explains:

```
$ cmake -S . -B build-coverage -G Ninja -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_PREFIX_PATH="$HOME/seastar-install-clang;$SEASTAR_SRC/build/release/_cooking/installed" \
    -DCMAKE_CXX_FLAGS="-fprofile-instr-generate -fcoverage-mapping \
        -mllvm -enable-single-byte-coverage -mllvm -conditional-counter-update" \
    -DCMAKE_EXE_LINKER_FLAGS=-fprofile-instr-generate
$ cmake --build build-coverage
$ export LLVM_PROFILE_FILE=$PWD/coverage/%p.profraw
$ build-coverage/shoal_unit_tests -- -c2 -m1G
$ (cd ../test && ./run 16 --shoal ../v16/build-coverage/shoal)
$ llvm-profdata merge -o shoal.profdata coverage/*.profraw
$ llvm-cov report build-coverage/shoal -object build-coverage/shoal_unit_tests \
      -instr-profile shoal.profdata -ignore-filename-regex=unit_tests.cc $PWD
```

Each process writes what it recorded to a file of its own, which
`LLVM_PROFILE_FILE` names, with `%p` for its process ID, since the tests
start many servers; `llvm-profdata` adds them up, and `llvm-cov report`
gives each file's coverage, of Shoal's own files (Seastar's weren't
compiled for coverage), and `llvm-cov show` the source, with whether
each line ran. Three things about a Seastar program matter here.

**The coverage is written when the program exits.** The coverage
runtime writes it in a function that runs when the process exits normally,
from `exit()` or a return from `main()`. A server that's killed, or
ends with `_exit()` or an abort, writes nothing. Shoal's tests killed,
with `SIGKILL`, the servers of their own that were still running when a
test ended, and the first time we measured Shoal's coverage, 34 of the
56 servers that the tests started left empty files:

| Servers left running are stopped by | Servers whose coverage was written | Lines | Functions |
|---|---:|---:|---:|
| `SIGKILL` | 22 of 56 | 81.2% | 82.2% |
| `SIGTERM` | 52 of 56 | 94.4% | 96.8% |

With `SIGTERM`, which makes Shoal stop cleanly, as
[chapter](#ch:clean-shutdown) explained, the tests now find that they
run 94.4% of Shoal's lines, not 81.2%: the code of the admin API, for
example, was run by tests whose servers were killed, and seemed never to
run at all. The 4 servers whose coverage is still lost are those that
tests kill on purpose, to check that Shoal survives a crash. A server
that ends with `_exit()` must write its coverage itself, by calling the
runtime's `__llvm_profile_dump()`, as ScyllaDB does before its own
`_exit()`; ScyllaDB's REST API can also call it, so that a test can
collect the coverage of a server that it's about to kill.

Stopping the servers cleanly found something else, too. Two tests' servers
now took 10 seconds each to stop: tests of Shoal's TLS port, which left
a client connected. When a Seastar TLS connection closes, Seastar sends
the TLS alert that closes the session, and then, in the background,
waits for the client's, up to `tls::options::bye_timeout`, 10 seconds
by default, and the reactor doesn't exit until that wait is over. A
server can't choose a shorter one, since `tls::listen()` takes no
options, though TLS doesn't require the wait at all. We reported it as
Seastar issue <https://github.com/scylladb/seastar/issues/3845>, and the
tests now close their clients.

**Shards share the counters.** By default, each piece of code gets a
counter, an ordinary global variable, which every shard's thread
increments, without synchronization: a count may come out lower than
the truth. Worse, shards that run the same code write the same memory,
whose cache line then moves from core to core at every increment, as
[chapter](#ch:shared-nothing) explained for any memory that shards
share. The option `-fprofile-update=atomic` makes the counts exact, and
the program much slower still. But coverage doesn't need counts, only
whether each piece of code ran. With `-mllvm
-enable-single-byte-coverage`, each piece of code gets a byte instead of
a counter, which it sets, and with `-mllvm -conditional-counter-update`,
it sets it only if it isn't set yet: after the first time, the shards
only read it, and each keeps the cache line in its own cache. We
measured Shoal's speed with each, with `valkey-benchmark`, as
[chapter](#ch:designing-a-sharded-application) did: 20 million `SET`s
and then `GET`s of random keys out of ten million, in pipelines of 64
requests on 64 connections, without the log. In thousands of requests a
second, the best of three runs:

| Build | 1 shard: `SET` | 1 shard: `GET` | 2 shards: `SET` | 2 shards: `GET` |
|---|---:|---:|---:|---:|
| No coverage | 2,960 | 3,328 | 3,993 | 4,438 |
| Counters (the default) | 2,661 | 3,071 | 1,816 | 2,284 |
| Atomic counters | 665 | 815 | 624 | 798 |
| Bytes | 2,662 | 3,074 | 1,996 | 2,349 |
| Bytes, set only once | 2,220 | 2,753 | 3,331 | 3,804 |

With counters, or bytes set every time, two shards serve fewer requests
than one did, because they fight over the cache lines. With bytes set
only once, the extra check costs 17% to 25% of the speed on one shard,
but two shards keep about 85% of theirs. Shoal's tests, which run it on two shards,
took 55 seconds with counters, 31 with bytes set once, and 29 without
coverage. Both `-mllvm` options are LLVM's own settings, not documented
options of Clang, so a new version of Clang may change them; we used
Clang 22. Clang's other kind of coverage, `-fsanitize-coverage`, made for
fuzzers, is also cheap, but marks the compiled code rather than the
source, and comes with no tool that reports it.

**Coverage has nothing to do with the build's checks.** A coverage
build can be any build: it tells which code ran, not whether it
ran correctly, so the `debug` build's shuffling and sanitizers add
nothing to it, and its slowness only makes the tests take longer. The
build above is Shoal's ordinary optimized one.

A line that no test runs is a question, not necessarily a fault: most of
Shoal's are the handling of errors that tests don't make happen, such as
a short write to the log, the kind of code that the error injection
earlier in this chapter is for. But some are a test that's missing.
`llvm-cov show` shows that when Shoal rewrites its log, the code that
writes the new log before the rewrite is done, when more than a
megabyte of it waits, never ran: no test rewrote a log that large
(exercise 5).

## Fuzzing and benchmarks {index="testing!fuzzing"}

A test checks the inputs that its author thought of. A *fuzzer* makes up
its own: libFuzzer, part of Clang, calls a function, again and again,
with inputs that it mutates, and keeps those that reach code it hadn't
reached before, until the function crashes, or a sanitizer finds a bug.
Seastar has a build mode for it, `fuzz`, which needs Clang, and its own
fuzz tests, in `tests/fuzz/`, such as one of `sstring`. A fuzz test is a
function that libFuzzer calls with each input:

```cpp
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // use data[0..size-1] as the input
    return 0;
}
```

It runs without a reactor, so it suits code that doesn't need one, such
as a parser of bytes, or a data structure, such as Shoal's table, whose
results a fuzz test can compare with a simpler one's (exercise 3).

A *benchmark* measures, rather than checks. Seastar's `perf_tests`
library, `seastar_perf_testing`, runs a function again and again, in a
reactor, for a while, and reports the time of each call, and what the
CPU did for it. Shoal 16 has two, in `perf_tests.cc`, written with
[`PERF_TEST`]{index="`PERF_TEST`" .main} and
[`PERF_TEST_F`]{index="`PERF_TEST_F`" .main}, from
`<seastar/testing/perf_tests.hh>`:

```{.cpp include="shoal/v16/perf_tests.cc" snippet="perf-parse"}
```

```{.cpp include="shoal/v16/perf_tests.cc" snippet="perf-table"}
```

`PERF_TEST_F`'s class is a fixture, whose constructor runs once, before
the test's calls, which are calls of a member function of a class
derived from it.
[`perf_tests::do_not_optimize()`]{index="`perf_tests::do_not_optimize()`"}
keeps the compiler
from dropping a computation whose result nobody uses. And the test
returns how many things it did, so that each is timed.

By default, `perf_tests` runs each test five times, for a second each
time, and a test whose future is ready at once, as both of these are, is
called again and again for that whole second, without yielding. The reactor's stall detector,
which [chapter](#ch:stalls) explained, would report that, every time, so
we raise its threshold to two seconds:

```
$ build/shoal_perf_tests -c1 -m1G --blocked-reactor-notify-ms 2000
single run iterations:    0
single run duration:      1.000s
number of runs:           5
number of cores:          1
random seed:              2111200820
start/stop overhead:      2.218µs (3.753µs)

test                     iters            runtime     allocs      tasks       inst     cycles   overhead
resp.parse_pipeline   23893700    41.79ns ± 0.13%      2.120      0.000    1046.34      191.6      0.000
table_fixture.find    13512000    73.77ns ± 1.04%      0.000      0.000     158.69      338.2      0.000
```

The columns are the time of each request or lookup, with its spread over
the runs, then its allocations, tasks, instructions and cycles, which
the library counts with the CPU's counters. A lookup in a table of a
million keys takes 74 ns, most of it a miss in the CPU's cache: the same
lookup of keys that stay in the cache, as our first version of the test
did, without `next`, took 12.4 ns, with the same 159 instructions. (Each
number is the best of three runs.) Run benchmarks without
`--overprovisioned`, unlike the tests above: a benchmark wants its
shards pinned to their cores, and a shard that sleeps when it's idle
takes longer to wake up. [Chapter](#ch:talking-between-shards) measured
a call to another shard at 950 nanoseconds, and at 5,355 with idle
polling turned off, as `--overprovisioned` turns it off. ScyllaDB has 53 such benchmarks, of its
own parts.

## Exercises

1. Add a unit test of `BLPOP`: one call of `execute()` blocks, waiting
   for a key, and another, an `LPUSH` to that key, wakes it up. How does
   the test make sure that the `BLPOP` waits before the `LPUSH` comes?

2. Write a small Seastar program that works in a `release` build and
   fails in a `debug` build, because it relies on the order in which two
   fibers run. Then make it correct.

3. Write a fuzz test of Shoal's `key_table`: interpret the input as a
   sequence of operations, insertions, lookups and erasures of keys,
   apply them to a `key_table` and to a `std::unordered_map`, and check
   that the two always agree. How many different keys should it use?

4. Add a benchmark of `execute_here()` executing a `GET` of a key that
   exists. Which takes longer, parsing a request or executing it?

5. Write a test that makes Shoal rewrite a log large enough for the
   rewrite to write its new log before it's done, which, as the section
   on coverage found, no test did, and check with a coverage build that
   the code now runs.
