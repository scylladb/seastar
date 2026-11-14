# Lessons from large Seastar code bases {#ch:lessons-from-large-code-bases}

ScyllaDB and Redpanda, the two largest programs built on Seastar
([chapter](#ch:who-uses-seastar)), each keep a library of utilities on
top of it: ScyllaDB in its `utils/` directory, Redpanda in `ssx/`,
`utils/` and `container/`. Much of what is in them has nothing to do
with a database or with Kafka, and some of it was written by both
projects, separately. That is the strongest evidence of what a large Seastar program needs beyond
Seastar itself, and what yours will probably need too.

This chapter goes through those utilities. Earlier chapters already
explained most of them, where their topic came up, and the first
section lists them, with where. The next sections explain the rest,
each as a design: the problem, the shape of a solution, and its
pitfalls, with code of our own, not copied from either project. Then
come two bugs from these code bases, each with a lesson, and what
Seastar doesn't have yet.

## What both of them wrote {index="Seastar!utilities that applications write"}

These are the utilities that both projects wrote, with the names that
each gave them:

| What, and where it is explained | ScyllaDB | Redpanda |
|---|---|---|
| Stopping on `SIGINT` and `SIGTERM` ([chapter](#ch:clean-shutdown)) | `stop_signal` | `stop_signal` |
| Clearing a huge container without a stall ([chapter](#ch:stalls)) | `utils::clear_gently()` | `ssx::async_clear()` |
| Containers that make no large allocation ([chapter](#ch:memory)) | `utils::chunked_vector` | `chunked_vector`, `chunked_hash_map` |
| A mutex ([chapter](#ch:coordination)) | a semaphore of one unit | `ssx::mutex` |
| Admission control ([chapter](#ch:coordination)) | `reader_concurrency_semaphore` | `adjustable_semaphore` |
| A service on one shard only ([chapter](#ch:talking-between-shards)) | services on shard 0 | `ssx::single_sharded` |
| Errors without exceptions ([chapter](#ch:errors)) | `utils::result_with_exception` | `result<>` |
| A watchdog for hung operations ([chapter](#ch:clean-shutdown)) | logs of a slow shutdown | `ssx::watchdog` |
| Error injection ([chapter](#ch:testing)) | `utils::error_injection` | `finjector` |
| A thread for blocking calls ([chapter](#ch:threads)) | `utils::alien_worker` | `ssx::thread_worker` |
| Retry with backoff (below) | `exponential_backoff_retry` | `retry_chain_node` |
| One run of an action for many triggers (below) | `serialized_action` | `ssx::work_queue`, in part |
| Waiting for operations of an earlier phase (below) | `utils::phased_barrier` | — |
| Configuration that changes while running (below) | `utils::updateable_value` | `config::binding` |

All of these are small: a class of a few dozen lines, or a few hundred.
None needs anything that Seastar doesn't offer; each only needs the
right Seastar primitives put together the right way, and each is easy
to get subtly wrong, which is why a project writes it once, in its
library, and not again in each place that needs it.

## Retry with backoff {index="retry with backoff"}

A request to another server can fail for reasons that pass: the server
is restarting, or overloaded, or a connection broke. The obvious answer
is to try again, and the obvious way, a loop that retries at once, makes
things worse: a server that failed because it was overloaded gets the
same load again, at once, and the loop spins on a shard's CPU for as
long as the failure lasts. The usual answer is
[*exponential backoff*]{index="exponential backoff" .main}: wait between
tries, and double the wait after each failure, up to a maximum, so that
a short failure is retried quickly, and a long one costs little.

Many clients often fail together, when the same server goes down, and
if they all wait the same delays, they all retry together, in waves of
load at the moment when the server comes back. So each client waits a
random part of the delay, which is called
[*jitter*]{index="jitter" .main}, spreading the retries out. Here is a
retry with backoff and jitter:

```{.cpp include="examples/lessons-from-large-code-bases/idioms.cc" snippet="retry"}
```

It waits with `sleep_abortable()`, so that an abort source, such as the
one that a server requests an abort from when it stops
([chapter](#ch:clean-shutdown)), ends the wait at once, and the server
doesn't wait for a retry's second-long sleep to stop. The random engine
is per thread, as a shard's state should be, and is seeded from
`std::random_device`: a `std::default_random_engine` constructed without
a seed starts from the same seed in every process, so every client
would draw the same "random" delays, and retry together after all.
(Our first version of this example made this mistake: every run waited
exactly the same delays.)

A function that fails three times, and then succeeds, prints:

```
retry: attempt 1 failed
retry: attempt 2 failed
retry: attempt 3 failed
retry: attempt 4 succeeded
```

Three pitfalls come with retrying:

- **Retry only what may succeed.** A request that failed because it was
  wrong, such as a syntax error, fails again, and a retry only delays
  the error. A real retry looks at the error, and gives up at once on
  those that won't pass.
- **Retry only what is safe to repeat.** A request whose reply was lost
  may have been done, and repeating it does it again: a write that
  appends, or a counter's increment, is done twice. Only an
  [*idempotent*]{index="idempotent request" .main} request, one that does the same when done twice, can be retried
  blindly.
- **Bound the time, not only the attempts.** A caller usually has a
  deadline, its own client's timeout, and retries that go on after it
  are wasted. Exercise 1 bounds the retries by a deadline.

ScyllaDB's `exponential_backoff_retry`, from 2015, doubles its delay up
to a maximum, without jitter. Redpanda's `retry_chain_node` adds jitter,
and a deadline that is shared by a tree of fibers: a fiber that starts
others gives them nodes of its own, whose deadlines can't be later than
its own, so that the retries of all the work done for one request end
by the request's deadline. Seastar itself has a retry only in its HTTP
client, whose `http::retry_strategy` decides whether a failed request
is retried ([chapter](#ch:http)).

## One run for many triggers {index="coalescing action"}

Some work must be done after an event, and the events come in bursts.
A server that keeps a list of its peers on disk must write it after
each change, and a change of the cluster may change it fifty times in a
second. Writing it fifty times is a waste, since only the last write
matters; writing it in fifty fibers at once is wrong, since they write
the same file. What is needed is an action that runs one at a time, and
that a burst of triggers runs only once or twice, so that its runs
[*coalesce*]{index="coalescing" .main}: a trigger promises only that a
run starts after it, and that it will learn when that run ended.

```{.cpp include="examples/lessons-from-large-code-bases/idioms.cc" snippet="coalescing"}
```

The waiters for the next run share one promise, a
[`shared_promise`]{index="`seastar::shared_promise`"}, from
`<seastar/core/shared_future.hh>`, whose future can be taken many times
([chapter](#ch:futures-and-promises)). A trigger that comes when no run
is promised creates the promise, and starts a run if none is in
progress; a trigger that comes while a run is promised but hasn't
started only takes the promise's future. `run()` takes the waiters of
the next run, runs the action, and tells them; if triggers came while
the action ran, it runs again, for them. Ten triggers at once, of an
action that takes 10 milliseconds, print:

```
coalescing: 10 triggers, 2 runs
```

Why two, and not one? The first trigger starts a run, and the run takes
its waiters at once, before the other nine come. The other nine come
while that run is in progress, after it took its waiters, so they can't
be sure that it saw their change, and they wait for the next run, which
serves all nine. That is the promise: a run that started after the
trigger.

The comment in `trigger()` marks a bug that our first version had. It
created the promise, started `run()`, and then returned the promise's
future, from `_next`. But a coroutine's body runs right away, when it's
called, until it first suspends ([chapter](#ch:coroutines)), and
`run()` takes `_next`, leaving it null, before its first `co_await`.
So `trigger()` returned a future from a null pointer, and crashed. The
`debug` build's AddressSanitizer ([chapter](#ch:testing)) showed where
at once. The cure is to take the future first.

A real version needs two more things. After `stop()`, a trigger should
fail, rather than start a run that nothing waits for, as ScyllaDB's
`serialized_action` does after it's stopped. And an error of the action
goes to all the waiters of the run that failed, which is what each of
them should see, since that run was the one meant for them; but a
trigger that comes after the failure starts a new run, and may succeed.
ScyllaDB's `serialized_action`, from 2017, works the way that our
`coalescing_action` does. Redpanda's `ssx::work_queue` runs work on one
fiber too, but runs every function that it's given, in order, rather
than coalescing them, which is what work that can't be skipped needs.

## Waiting for operations of an earlier phase {index="phased barrier"}

A gate ([chapter](#ch:coordination)) waits for every operation that
holds it, and then refuses new ones, which is what stopping a service
needs. Sometimes a program needs to wait only for the operations that
began before some moment, while new ones go on: after a change of the
cluster's layout, a server must know when no request still uses the old
layout, before it deletes the data that moved elsewhere, and new
requests, which use the new layout, mustn't be refused, nor waited for.
A [*phased barrier*]{index="phased barrier" .main} does this, with a
gate per phase:

```{.cpp include="examples/lessons-from-large-code-bases/idioms.cc" snippet="barrier"}
```

An operation holds the current phase's gate. `advance_and_await()`
replaces the gate with a new one, for the operations that begin from now
on, and closes the old one, which waits for the operations that hold it.
The gate is a shared pointer, so that an operation's holder keeps its
gate alive after the barrier replaced it. In the example, operation `a`
begins, then the barrier advances, then operation `b` begins, and takes
longer than `a`:

```
barrier: ["a ended", "barrier passed", "b ended"]
```

The barrier passed when `a` ended, without waiting for `b`. ScyllaDB's
`utils::phased_barrier`, from 2015, is the same idea, with a phase
number that an operation can remember, and a name for each gate, for
the logs. ScyllaDB uses it for the case above, among others: each
request holds the barrier of the version of the cluster's layout that it
uses, and a change of layout, before its next step, waits on the
barrier until no request uses an older layout. Redpanda has nothing of the kind.

The pitfall is the same as a gate's: an operation that never ends keeps
every later barrier from passing, so an operation that waits for
something, such as a client, should also have a timeout, or an abort
source.

## Configuration that changes while running {index="configuration!live updates"}

A server's operators want to change its settings without restarting it:
a limit of memory, a timeout, the shares of a scheduling group. With
Seastar, that brings two problems. The setting is read on every shard,
and shards share nothing ([chapter](#ch:shared-nothing)), so each needs
its own copy, and a change must reach every shard. And code that copied
the setting when it started, into a member or a local variable, never
sees the change.

Both projects solved this the same way. Each shard has its own copy of
the configuration, which one shard updates on every shard, with
`invoke_on_all()`, when the configuration changes. Code that needs a
setting doesn't copy its value: it holds a small object, ScyllaDB's
`updateable_value<T>` or Redpanda's `config::binding<T>`, which the
configuration of its shard knows about, and updates in place when the
setting changes. Code that must do something when a setting changes,
not only see the new value, registers a callback with it: a semaphore
whose number of units is a setting must be given more units, or have
some taken away, and a scheduling group whose shares are a setting must be
given the new shares ([chapter](#ch:scheduling-groups)). Seastar's
semaphore has no method to change its size, but `signal()` adds units,
and `consume()` takes them away, even below zero, so that the
semaphore admits nothing until enough units come back; Redpanda's
`adjustable_semaphore` wraps a semaphore with a `set_capacity()` that
does this.

Both are used widely: ScyllaDB's code mentions `updateable_value` about
490 times, and Redpanda's mentions `binding<` about 1,000 times. The
pitfall is a setting that can't change, because the code that uses it
read it once: the operator changes it, the server says it accepted the
change, and nothing happens. A test that changes each live setting and
checks its effect catches it.

## Two bugs and their lessons {index="bugs, lessons from"}

### Locking every shard {index="deadlock!between shards"}

In 2026, ScyllaDB's tests began to hang, now and then, while a node
joined a cluster.[^lock-tables] The cause was a lock: ScyllaDB keeps a lock on each
shard that protects the shard's list of tables, and a change of tables
takes the lock on every shard, with `invoke_on_all()`. That takes the
locks in parallel. As [chapter](#ch:talking-between-shards) explained,
`invoke_on_all()` sends a message to every shard at once, and doesn't
wait for one before it sends the next.

That was fine while one fiber at a time took the locks. Then a change
added a second fiber that did, at the same time as the first, when a
node started. Two fibers took the locks in parallel, and nothing made
each shard grant its lock to the same fiber: on one shard, the first
fiber's message came first, and on another, the second fiber's. Each
fiber then held some of the locks, and waited forever for the others,
which the other fiber held. This is a classic deadlock, of two locks
taken in different orders. It was most likely in the `debug` build,
whose tasks run in a shuffled order ([chapter](#ch:testing)), but,
as its developers explained, possible in a `release` build too, only
much less likely.

The fix took shard 0's lock first, and only then the others, in
parallel: whichever fiber gets shard 0's lock has no competitor for the
rest. Its developers reproduced the deadlock with delays, added in a
test build, that made each fiber slow to take a different shard's lock;
exercise 2 does the same.

The lesson is the old one for locks, which Seastar doesn't change: locks
taken together need an order. What Seastar adds is that "taking the
locks of all shards" looks like one operation, with `invoke_on_all()`,
and is many, done in parallel and in no particular order.

### A container that grew too large {index="large allocations"}

In 2017,[^chunked-vector] ScyllaDB kept lists that grow with the size of its files in
`std::deque`, which, in GCC's standard library, keeps its elements in
chunks of 512 bytes,
so that it never needs much memory in one piece for them. But it keeps
an array of pointers to its chunks, one for each 512 bytes of elements,
and that array grows with the deque. With large enough files, it grew
past 256 KB, which ScyllaDB's developers considered too large an
allocation to be safe. As [chapter](#ch:memory) explained, Seastar's
allocator serves a large allocation from a block of contiguous memory,
which a shard's fragmented memory may not have, however much is free.

The fix was a container of their own, `utils::chunked_vector`, which
keeps elements in chunks of 128 KB, and makes its array of pointers to
them small. It is now one of ScyllaDB's most used containers, mentioned
about 1,800 times. Redpanda wrote a `chunked_vector` of its own, which
its code mentions about 4,300 times, and a `chunked_hash_map`. The lesson
is that a container whose size depends on the data, rather than on the
program, will one day be large: in a server that runs for months, any
`std::vector`, string or hash table that grows with the data should be a
chunked one, from the start.

[^lock-tables]: ScyllaDB commit `ebaf536449`, "replica/database: fix
    cross-shard deadlock in lock_tables_metadata()", April 2026, pull
    request <https://github.com/scylladb/scylladb/pull/29678>.

[^chunked-vector]: ScyllaDB commit `3ba2c0652d`, "utils: add a new
    container type chunked_vector", August 2017.

## What Seastar doesn't have yet {index="Seastar!what it lacks"}

What both projects wrote for themselves is what Seastar doesn't have
yet. Some of it may move into Seastar one day; meanwhile, a new project
must write it too, or borrow it. Here is what is missing, as of
November 2026:

- **Chunked containers.** Seastar has `chunked_fifo`, a queue
  ([chapter](#ch:memory)), and nothing else. Issue 2133, "chunked_map in
  seastar", opened by a Redpanda engineer, asks for a hash map that
  makes no large allocations; it's still open.
- **An evictable, compactable allocator.** ScyllaDB's cache lives in its
  [log-structured allocator]{index="log-structured allocator"}, which can move objects to defragment
  memory, and evict them when memory runs short ([chapter](#ch:memory)
  described the idea). It's part of ScyllaDB, under ScyllaDB's own
  license, so other projects can't freely use it.
- **A JSON parser.** Seastar can write JSON, for its HTTP API
  ([chapter](#ch:http)), but not parse it. Both projects use RapidJSON,
  which ScyllaDB wraps in its own `rjson`.
- **The utilities of this chapter.** A retry with backoff, except for
  the HTTP client's; an action whose runs coalesce; a phased barrier;
  live configuration.
- **A worker thread for blocking calls.** Seastar has the functions
  that a thread of its own uses to send work to the shards, but not the
  worker itself, with its queue and its stopping
  ([chapter](#ch:threads)); ScyllaDB and Redpanda each wrote one.
- **Error injection.** Seastar can make memory allocations fail, but
  has nothing for making other operations fail or wait on purpose in a
  test ([chapter](#ch:testing)); both projects wrote their own.
- **Detecting deadlocks.** A deadlock of semaphores or locks, such as
  the one above, leaves fibers waiting forever, with no message.
  Seastar's issue 1777, "dead lock detection", asks for it; it's still
  open. Meanwhile, timeouts on waits, and the watchdogs of
  [chapter](#ch:clean-shutdown), at least report one.
- **A throwing assertion.** An assertion that reports an internal
  error and throws, failing only the operation that found the bug,
  rather than aborting the server, as [chapter](#ch:errors)'s `CHECK()`
  did. It takes a few lines; ScyllaDB's, `throwing_assert()`, is used
  about 130 times.

And so that you don't write what Seastar does have: `sleep_abortable()`
and abort sources ([chapter](#ch:time)), `shared_promise` and
`shared_future`, gates and named gates, `rwlock` and `shared_mutex`
([chapter](#ch:coordination)), `with_timeout()`, and `chunked_fifo`.

## Exercises

1. Change `retry_with_backoff()` to give up at a deadline, rather than
   after a number of attempts. It should give up as soon as it knows
   that its next wait would end after the deadline, rather than sleep
   only to fail. Test it with a function that always fails, and a
   deadline 200 milliseconds away. What part of the time does the
   deadline still not bound?

2. Write a lock of every shard: a `sharded<>` service whose instance on
   each shard holds a semaphore of one unit, and a function that takes
   all of them in parallel, with `sharded::map()`. Run two fibers that
   take the locks at once. Make them deadlock every time, by delaying
   each fiber's wait for a different shard's lock by 20 milliseconds,
   and give each wait a timeout, so that the deadlock ends and you can
   see it. Then take shard 0's lock first, as ScyllaDB's fix did, and
   check that the same delays don't deadlock it, with 2 shards and
   with 4.
