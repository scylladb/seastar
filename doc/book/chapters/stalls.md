# Stalls: finding and preventing them {#ch:stalls}

A *stall* is a task that runs too long without yielding, and holds up
everything else on its shard. [Chapter](#ch:how-seastar-runs-your-code)
met the first ones: Shoal's `KEYS`, which looked at every key without
yielding, and a rehash inside `std::unordered_map`, in code that Shoal
didn't write. It also met the stall detector, which reports a task that
runs longer than 25 milliseconds, with a backtrace, and showed that it
can't see a thread that blocks in a system call. This chapter is about
the stalls that remain after the obvious ones are fixed: where they come
from, how to find them before users do, and how to prevent them, in code
that we write and in code that we don't.

A stall costs more than its own length. While it lasts, no other task on
its shard runs: the shard reads no requests, answers none, fires no
timers, and sends no replies to other shards, whose requests wait for it
too. The I/O scheduler of [chapter](#ch:io-scheduler) dispatches its
requests from the reactor's loop, so the disk waits as well, and a stall
of 50 milliseconds in one request is 50 milliseconds added to every
request that arrives at the shard during it.

## Where stalls come from {index="stall!sources"}

Most stalls aren't in loops that anyone would suspect, but in ordinary
operations on large data. This program times some of them, each run on
a shard, which can do nothing else meanwhile:

```{.cpp include="examples/stalls/sources.cc" snippet="timed"}
```

```{.cpp include="examples/stalls/sources.cc" snippet="sources"}
```

On one shard, the best of three runs:

```
destroying a vector of a million strings           5.5 ms
destroying an unordered_map of a million keys      3.9 ms
sorting ten million numbers                      630.7 ms
copying them (40 MB)                               3.8 ms
formatting them into a log message               219.0 ms
throwing and catching 100,000 exceptions          34.2 ms
writing to 512 MB of fresh memory                237.0 ms
writing to the same 512 MB again                   5.1 ms
```

Each of these is a single call, which a program makes without a second
thought. Sorting ten million numbers stalls the shard for more than half
a second. Formatting them into one log message, which a debugging line
can do by accident, stalls it for a fifth of a second. A burst of
exceptions, such as a failure that every request in a large batch
reports, costs about 340 nanoseconds for each. And the first write to
memory that the shard never used costs more than forty times as much as
a write to memory that it did: the kernel gives a process its memory
only when the process first touches it, a page at a time, and zeroes
each page first.
A shard that is still filling its memory, after it starts, or after it
grew something large, pays this on every new page, as
[chapter](#ch:threads)'s first thousand threads did with their stacks.
The copy shows it too: 3.8 milliseconds in the best run, but 31 and 49
in the other two, in which the copy got memory that the shard hadn't
used before.

The two destructions were quick, and the reason is instructive: their
million strings and nodes were allocated one after another, and freed in
the same order, so the freeing walked through memory in order, and the
processor's caches kept up. A container whose elements were allocated
at different times, mixed with other data, or that frees them in a
different order, such as a hash table with random keys, pays a cache
miss for each, as [chapter](#ch:memory) measured: with a million random
keys, the same table took four times as long to destroy, and the table
below, with a string of 100 characters for each of two million random
keys, took 137 milliseconds. Exercise 2 measures it.

## Preventing them {index="stall!preventing"}

The fix, for code that we write, is to make long work yield, which
[chapter](#ch:how-seastar-runs-your-code) did for `KEYS`:
`coroutine::maybe_yield()` in a loop, which yields only when the task
quota is used up, so that it costs almost nothing when there's no need
to yield. The same goes for destroying something large, which a
destructor can't do gradually, since it can't wait. Instead, the
container is emptied first, a few elements at a time, by a coroutine
that yields as it goes:

```{.cpp include="examples/stalls/gentle.cc" snippet="clear-gently"}
```

This program destroys a table of two million random keys, each with a
string of 100 characters, once at once and once gradually, while a timer
that should fire every millisecond notes how late it came:

```{.cpp include="examples/stalls/gentle.cc" snippet="compare"}
```

```
at once: the heartbeat was up to 136.9 ms late
gradually, in 169 ms: the heartbeat was up to 0.0 ms late
```

Seastar has no `clear_gently()` of its own, and the programs built on it
each wrote one. ScyllaDB's `utils::clear_gently()` has overloads for
dozens of types, from standard containers to its own, and is used in 46
of its files; Redpanda's `ssx::async_clear()` does the same for its hash
maps. [Chapter](#ch:concurrency-within-a-shard)'s `FLUSHALL ASYNC`
is the same idea in Shoal: move the data out, at once, and destroy it
gradually, in the background.

For the rest, the earlier chapters already have the tools. Containers
that never make a large allocation, and grow without rehashing everything
at once, such as [chapter](#ch:memory)'s table of small tables, have no
growth stalls. Long synchronous code, such as a parser, can run in a
Seastar thread, and call `thread::maybe_yield()` deep inside its loops
([chapter](#ch:threads)). And work that can't yield at all can run in a
worker thread outside Seastar, also as [chapter](#ch:threads) showed.

### Code you didn't write {index="stall!in libraries"}

The hardest stalls are in code that can't be changed to yield: a
library function, or the standard library's own, such as the sort, the
formatting and the destructors above. There are three ways around them.
One is to give such code less at a time: sort in pieces, which yield in
between, and merge them, as exercise 5 does; format a large value into a
log message only up to a limit; and keep containers small, by dividing
large data into many smaller ones, as Shoal's table of small tables
does. Another is to run it in a worker thread, as
[chapter](#ch:threads)'s worker does, if its input can be handed over,
and its result taken back, cheaply. The third is to replace it, as
Shoal replaced `std::unordered_map` with a table that doesn't stall: this
is the stall that [chapter](#ch:how-seastar-runs-your-code) found in
Shoal's `SET`, deep inside the standard library's rehash, and that
version 12 removed ([chapter](#ch:memory)).

## Finding them {index="stall!finding"}

The stall detector of [chapter](#ch:how-seastar-runs-your-code) reports
tasks that run longer than 25 milliseconds, the threshold of
`--blocked-reactor-notify-ms`. A server can also change it while it
runs, which ScyllaDB and Redpanda do from a configuration option that
can be changed without a restart. This needs
[`engine()`]{index="`seastar::engine()`" .main}, from
`<seastar/core/reactor.hh>`, which returns the calling shard's
[`reactor`]{index="`seastar::reactor`"}. Applications rarely need it
nowadays: almost everything that it once did has a function of its own,
such as `this_shard_id()`, `open_file_dma()` and `listen()`, and what
remains of it are tuning settings and statistics, the stall threshold
among them:

```{.cpp include="examples/stalls/threshold.cc" snippet="threshold"}
```

`busy()` keeps the shard busy for 15 milliseconds. Only the lower
threshold reports it:

```
threshold 25 ms: working for 15 ms
INFO  [shard 0:main] seastar - updated: blocked-reactor-notify-ms=5
threshold 5 ms: working for 15 ms
Reactor stalled for 14 ms on shard 0, in scheduling group main. Backtrace: ...
```

The threshold is per shard: `update_blocked_reactor_notify_ms()` changes
the calling shard's, so a server that changes it everywhere calls it on
every shard. A lower threshold finds smaller stalls, but each report
costs a backtrace, and a busy server can produce many reports, which the
limit of five a minute, of [chapter](#ch:how-seastar-runs-your-code),
keeps from flooding its log.

The stall detector only reports stalls; it doesn't count them. Seastar's
metrics do, more finely: every time the tasks of a scheduling group run
for more than twice the task quota, 1 millisecond by default, Seastar
adds the time to a histogram, `reactor_stalls`, and how much it exceeded
the quota to a counter of the group,
`scheduler_time_spent_on_task_quota_violations_ms`, and counts the
detector's reports in `stall_detector_reported`.
[Chapter](#ch:observability) shows how to watch them. A rising counter
says which group stalls, and how much, long before the stalls are long
enough for the detector to report.

Tests are a good place to find stalls, since they run the code on purpose
with large data. But beware of what a test can't see: debug builds,
in which many tests run, are many times slower than release builds, so
their stalls are many times longer, and many projects raise the
threshold in tests rather than lower it, to keep the noise out of their
logs. ScyllaDB runs its C++ unit tests with
`--blocked-reactor-notify-ms=2000000`, which turns the detector off. A
stall hunt therefore wants a release build, with data as large as
production's.

## A stall hunt in Shoal {index="Shoal!stall hunt"}

Shoal's tests pass, and the benchmarks of
[chapter](#ch:designing-a-sharded-application) and
[chapter](#ch:memory) showed no stalls. But they used small values,
and small lists. This hunt used a list of five million elements of 100
characters each, made with
`valkey-benchmark`, and then deleted it, with Shoal's stall threshold
lowered to 5 milliseconds:

```
$ shoal -c1 --blocked-reactor-notify-ms 5 --appendonly no &
$ valkey-benchmark -t rpush -n 5000000 -d 100 -P 64 -q
RPUSH: 2805836.25 requests per second, p50=0.295 msec
$ valkey-cli DEL mylist
(integer) 1
```

The `DEL` took about 50 milliseconds, and Shoal's log had three
reports, of one stall:

```
Reactor stalled for 13 ms on shard 0, in scheduling group main. Backtrace: ...
Reactor stalled for 25 ms on shard 0, in scheduling group main. Backtrace: ...
Reactor stalled for 47 ms on shard 0, in scheduling group main. Backtrace: ...
```

Decoded into function names, as [chapter](#ch:debugging-and-profiling)
shows, the backtraces end in the stall detector, called from the
allocator, called from the destructor of the list:

```
seastar::memory::small_pool::trim_free_list()
seastar::memory::small_pool::deallocate(void*)
std::__new_allocator<seastar::basic_sstring<...>>::deallocate(...)
list_value::~list_value()
(anonymous namespace)::del_here(store&, ...)
(anonymous namespace)::del_on_shard(store&, ...)
execute_here(store&, command const*, ...)
server::serve(...)
```

`DEL` destroys the list at once, five million elements and their
strings, as `FLUSHALL` did before
[chapter](#ch:concurrency-within-a-shard) gave it `ASYNC`. Valkey doesn't
stall here: its `DEL` frees large values in the background, in a thread
of its own, by default (its `lazyfree-lazy-user-del` option). Exercise 1
fixes Shoal's.

## Exercises

1. Make Shoal's `DEL` destroy a large list gradually, in the background,
   as `FLUSHALL ASYNC` destroys the old table, while the key disappears at
   once. Which other commands can destroy a large list? Repeat the stall
   hunt, and check that the stall is gone.

2. In `sources.cc`, insert the million keys of the `unordered_map` in a
   random order, rather than 0, 1, 2 and so on, and measure its
   destruction again. Why is it so much slower?

3. `clear_gently()` assumes that `erase(begin())` is cheap. For which
   standard containers is it? Write a version for `std::vector` that
   doesn't take quadratic time, and measure it on a vector of ten million
   strings.

4. In `threshold.cc`, find the shortest work that the stall detector
   reports with a threshold of 5 milliseconds. Explain the result with
   [chapter](#ch:how-seastar-runs-your-code)'s account of when the
   detector checks.

5. Sort ten million numbers without stalling: sort pieces of 100,000,
   yielding in between, and then merge them, yielding as you go. How much
   longer does the whole sort take than `std::ranges::sort()`?
