# Seastar threads {#ch:seastar-threads}

Seastar has a third way to write code that waits, besides coroutines and
the continuations of [chapter](#ch:continuations): the *Seastar thread*,
or `seastar::thread`.
Despite its name, a Seastar thread isn't an operating-system thread,
like those of [chapter](#ch:outside-world). It never runs in parallel
with the rest of its shard. It runs on the shard, in the shard's own
operating-system thread, as one more fiber
([chapter](#ch:concurrency-within-a-shard)), and like every fiber, it
runs only when the reactor gives it the shard, and gives the shard back
when it waits. What a Seastar thread has that a coroutine doesn't is a
stack of its own. In it, waiting for a future is an ordinary function
call, so the code that waits needn't be asynchronous: it can be an
ordinary function, called by other ordinary functions.

Avi Kivity added threads to Seastar in May 2015, between continuations
and coroutines, as "a blockable thread of execution". Coroutines have
since replaced them in most code, but they're still the right tool for
a few kinds of code, which this chapter shows: code that runs once, such
as a server's startup and shutdown, tests, and existing synchronous
code, such as a parser, that runs too long and must yield, but can't be
rewritten to use futures throughout. In this chapter, "thread" alone always means a Seastar
thread.

## Waiting in an ordinary function {index="Seastar thread"}

[`async()`]{index="`seastar::async()`" .main}, from
`<seastar/core/thread.hh>`, runs a function in a new *Seastar thread*,
and returns a future of its result. The thread is an object of the class
[`seastar::thread`]{index="`seastar::thread`" .main}, from the same
header, which a program can also create itself, and wait for with its
`join()` method, but `async()` is simpler: it creates the thread, keeps
it alive until the function returns, and gives back the function's
result. In a Seastar thread, a future's
`get()` method can wait: if the future isn't available yet, `get()`
suspends the thread, lets the shard run other tasks, and returns when the
future is resolved, as `co_await` does in a coroutine. So the function
that waits needn't be asynchronous:

```{.cpp include="examples/seastar-threads/basics.cc" snippet="wait"}
```

This prints `the answer, 42, took 100 ms`. `slow_answer()` is an ordinary
function, which returns an `int`, not a future, and waits inside. It works
because a Seastar thread has a stack of its own. When `get()` must wait,
the thread saves where it is, on its stack, and switches back to the
reactor's stack, which goes on running other tasks; when the future is
resolved, a task switches back to the thread's stack, and `get()`
returns. A coroutine can't do this: it has no stack of its own, only a
frame, so every function between it and the wait must be a coroutine
too. A thread is a fiber with a stack ([chapter](#ch:concurrency-within-a-shard)),
and while it runs, it's a task like any other, of the scheduling group
in which it was created. Outside a Seastar thread, `get()` can't wait, as
[chapter](#ch:futures-and-promises) showed.

`async()` can also take, before the function, a
[`thread_attributes`]{index="`seastar::thread_attributes`" .main}, a
struct with two fields: `sched_group`, a scheduling group for the thread
to run in instead, and `stack_size`, the size of its stack, which the
next section discusses:

```cpp
thread_attributes attr;
attr.sched_group = maintenance;
attr.stack_size = 32 * 1024;
co_await async(attr, [] { ... });
```

ScyllaDB starts its CQL server this way, in a thread that runs in the
server's scheduling group, so that the tasks that the server starts run
in that group too.

## What a thread costs

A thread's stack is the price. It's 128 KB by default, which
`stack_size` can change, allocated from the shard's memory, like
any other allocation: a large allocation, in
[chapter](#ch:memory)'s terms, so 100,000 threads that exist at the
same time would need 12.8 GB. And it's fixed: a thread whose calls go deeper overflows it.
Seastar's debug, sanitize and dev builds put a guard page at the end of
each stack, so that an overflow crashes immediately, but release builds
don't, and there an overflow writes over whatever lies next to the
stack in the shard's memory. This program measures what a thread costs,
against a coroutine, each starting, yielding once, and ending:

```{.cpp include="examples/seastar-threads/basics.cc" snippet="cost"}
```

```{.cpp include="examples/seastar-threads/basics.cc" snippet="rounds"}
```

The best of five runs, on one shard:

```
round 1: a thread 7032 ns, a coroutine 44 ns
round 2: a thread 600 ns, a coroutine 34 ns
round 3: a thread 628 ns, a coroutine 36 ns
```

The first thousand threads cost 7 microseconds each, most of it in the
first touch of their fresh stacks, 128 MB of them: with that memory
touched beforehand, they cost as little as the later ones. Those, whose
stacks reuse the memory, cost about 0.6 microseconds, still 17 times as
much as a coroutine.

## Where threads fit

So threads aren't for code that runs for every request: coroutines
replaced them there, and ScyllaDB, for example, calls `async()` in only
about a hundred places outside its tests. But three kinds of code still
use them. One is code that runs once, where
convenience matters more than a microsecond, such as a server's startup
and shutdown: ScyllaDB's `main()` runs in a Seastar thread. Another is
tests: ScyllaDB has 1,163 test cases that run in Seastar threads, against
2,003 that are coroutines, and Redpanda has 906. The third is existing
synchronous code that runs too long, at the end of this chapter.

## Waiting in a destructor

A Seastar thread can also wait in a destructor, which a coroutine can't,
since a destructor can't be a coroutine. That's what
[`deferred_stop`]{index="`seastar::deferred_stop`" .main}, from
`<seastar/util/closeable.hh>`, and
[`deferred_close`]{index="`seastar::deferred_close`" .main} are for, as
[chapter](#ch:clean-shutdown) promised: an object whose destructor calls
`stop()`, or `close()`, on the object that it was given, and waits for
the returned future with `get()`. So services stop in the reverse order
of starting, whichever way the function ends:

```{.cpp include="examples/seastar-threads/deferred.cc" snippet="deferred"}
```

Run once normally, and once with `fail`, in a Seastar thread, it
prints:

```
database started
server started
server stopped
database stopped
database started
server started
server stopped
database stopped
run: something failed
```

The object's `stop()` must be `noexcept`: it's called from a
destructor, which mustn't throw. ScyllaDB's `main()` stops its services
this way, with a function of its own, 113 times.

## Long synchronous code {index="Seastar thread!for long computations"}

Some code is neither asynchronous nor quick: a parser, a compressor, a
library that walks a large structure. On a shard, it stalls everything
else for as long as it runs. In a Seastar thread, it can call
[`thread::maybe_yield()`]{index="`seastar::thread::maybe_yield()`" .main}
every so often, which yields if the task quota is used up, as
`coroutine::maybe_yield()` does in a coroutine
([chapter](#ch:how-seastar-runs-your-code)):

```{.cpp include="examples/seastar-threads/long-work.cc" snippet="count-primes"}
```

The program runs it in a Seastar thread, once without yielding and once
with, while a timer that should fire every millisecond notes how late it
came:

```
not yielding: 348513 primes in 880 ms; the heartbeat was up to 878.8 ms late
yielding: 348513 primes in 883 ms; the heartbeat was up to 0.0 ms late
```

The computation took as long either way, but yielding left the shard
free for others. Note what made this possible: `count_primes()` calls
`maybe_yield()` from deep inside its loops, which no coroutine could do
without every function around the loop being a coroutine too. ScyllaDB's
Alternator parses large JSON requests this way: it runs the RapidJSON
parser in a Seastar thread, with a handler that may yield at the start
of each object and array, so a request of many megabytes doesn't stall
its shard.

## Exercises

1. Rewrite `run()` in `deferred.cc` as a coroutine. Why can't it use
   `deferred_stop`? Make it stop its services in the reverse order,
   whichever way it ends, without a Seastar thread.

2. Rewrite `count_primes()` from `long-work.cc` as a coroutine that
   yields with `coroutine::maybe_yield()`, and run it as the chapter did.
   Then move the test of whether one number is prime into a function of
   its own, which yields with `maybe_yield()` before it returns. What
   must the function become, and what does that cost?
