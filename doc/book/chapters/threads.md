# Seastar threads and the outside world {#ch:threads}

Everything so far ran on the shards, in tasks: coroutines, which wait by
suspending, and give the shard back at every `co_await`. Two kinds of
code don't fit. One is code that waits but isn't written as coroutines:
code that calls a function, and needs its result, without being
asynchronous itself. For it, Seastar has *threads* of its own, with a
stack each, in which waiting for a future is an ordinary function call.
The other is code that must not run on a shard at all: a library that
blocks, as [chapter](#ch:case-for-seastar) warned, a computation that
runs for seconds, or a whole existing program built on operating-system
threads. It runs in threads that aren't Seastar's, and this chapter shows
how they and the shards talk. It ends with Seastar's way to run other
programs.

## Seastar threads {index="Seastar thread"}

[`async()`]{index="`seastar::async()`" .main}, from
`<seastar/core/thread.hh>`, runs a function in a new *Seastar thread*,
and returns a future of its result. In a Seastar thread, a future's
`get()` method can wait: if the future isn't available yet, `get()`
suspends the thread, lets the shard run other tasks, and returns when the
future is resolved, as `co_await` does in a coroutine. So the function
that waits needn't be asynchronous:

```{.cpp include="examples/threads/basics.cc" snippet="wait"}
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
in which it was created, or of the one that its
[`thread_attributes`]{index="`seastar::thread_attributes`"} choose.
Outside a Seastar thread, `get()` can't wait, as
[chapter](#ch:futures-and-promises) showed.

A thread's stack is the price. It's 128 KB by default, which
`thread_attributes` can change, allocated from the shard's memory, like
any other allocation: a large allocation, in
[chapter](#ch:memory)'s terms, so 100,000 threads at once would need
12.8 GB. And it's fixed: a thread whose calls go deeper overflows it.
Seastar's debug, sanitize and dev builds put a guard page at the end of
each stack, so that an overflow crashes at once, but release builds
don't, and there an overflow writes over whatever lies next to the
stack in the shard's memory. This program measures what a thread costs,
against a coroutine, each starting, yielding once, and ending:

```{.cpp include="examples/threads/basics.cc" snippet="cost"}
```

```{.cpp include="examples/threads/basics.cc" snippet="rounds"}
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

### Where threads fit

So threads aren't for code that runs for every request: coroutines
replaced them there, and ScyllaDB, for example, calls `async()` in only
about a hundred places outside its tests. But three kinds of code still
use them. One is code that runs once, where
convenience matters more than a microsecond, such as a server's startup
and shutdown: ScyllaDB's `main()` runs in a Seastar thread. Another is
tests: ScyllaDB has 1,163 test cases that run in Seastar threads, against
2,003 that are coroutines, and Redpanda has 906. The third is long
synchronous code, below.

A Seastar thread can also wait in a destructor, which a coroutine can't,
since a destructor can't be a coroutine. That's what
[`deferred_stop`]{index="`seastar::deferred_stop`" .main}, from
`<seastar/util/closeable.hh>`, and
[`deferred_close`]{index="`seastar::deferred_close`"} are for, as
[chapter](#ch:clean-shutdown) promised: an object whose destructor calls
`stop()`, or `close()`, on the object that it was given, and waits for
the returned future with `get()`. So services stop in the reverse order
of starting, whichever way the function ends:

```{.cpp include="examples/threads/deferred.cc" snippet="deferred"}
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

### Long synchronous code {index="Seastar thread!for long computations"}

Some code is neither asynchronous nor quick: a parser, a compressor, a
library that walks a large structure. On a shard, it stalls everything
else for as long as it runs. In a Seastar thread, it can call
[`thread::maybe_yield()`]{index="`seastar::thread::maybe_yield()`" .main}
every so often, which yields if the task quota is used up, as
`coroutine::maybe_yield()` does in a coroutine
([chapter](#ch:how-seastar-runs-your-code)):

```{.cpp include="examples/threads/long-work.cc" snippet="count-primes"}
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

## Seastar's own threads {index="system-call thread"}

A Seastar process has more threads than shards. These are the threads
of Shoal, on two shards:

```
$ cat /proc/$(pidof shoal)/task/*/comm
shoal
shoal-ust
shoal-ust
reactor-1
syscall-0
syscall-1
iou-wrk-1896420
```

`shoal`, the program's first thread, is shard 0's reactor, and
`reactor-1` is shard 1's. The two `-ust` threads are LTTng's, the
tracing library, which Seastar includes unless it's configured with
`--disable-lttng`; `iou-wrk` is a thread of the kernel's, which
io_uring uses. And each shard has a *system-call thread*, `syscall-0`
and `syscall-1`. Some system calls have no form that doesn't block:
opening a file, `stat()`, renaming, listing a directory, starting a
process. Seastar sends them to the shard's system-call thread, which
makes the call, and the reactor learns that it's done through an event,
so the shard goes on running meanwhile. That's why `open_file_dma()`
returns a future, although opening a file is a single system call.
Seastar counts these calls in the metric
`reactor_io_threaded_fallbacks`, by reason. The system-call thread is
Seastar's own: a program can't give it code of its own to run. A
program's blocking code needs threads of its own.

## Threads that aren't Seastar's {index="alien thread"}

A thread that a program starts itself, with `std::thread`, is outside
Seastar, which calls it an *alien*. It can block: it isn't a shard, and
nothing waits for it. But it can't call anything of Seastar's that needs
a reactor, which is nearly everything: sleeping, timers, `submit_to()`,
even `this_smp_shard_count()`, which crashed the first version of the
example below. Its memory comes from the C library's `malloc()`, not
from a shard's ([chapter](#ch:memory)); memory of a shard that it frees
goes back to that shard, through the list that
[chapter](#ch:memory) described.

What an alien thread can do is send functions to the shards, with two
functions from `<seastar/core/alien.hh>`.
[`alien::run_on()`]{index="`seastar::alien::run_on()`" .main} sends a
function to a shard, to run there as a task, and returns at once; the
function must be `noexcept`, since nothing could receive its exception.
[`alien::submit_to()`]{index="`seastar::alien::submit_to()`" .main}
sends a function that returns a future, and returns a `std::future` of its
result, which the alien thread can wait for, blocking itself. This is the
one place where a Seastar program meets `std::future`, which
[chapter](#ch:coroutines)'s corner said has no place in it: an alien
thread may block, so it may wait for one. Both take an
[`alien::instance`]{index="`seastar::alien::instance`"}, which stands for
the Seastar program, from `app_template::alien()`:

```{.cpp include="examples/threads/alien.cc" snippet="outside"}
```

The shard that starts the alien thread must not block either, so it
doesn't wait for the thread with `join()`, which would block it until the
thread ends, but checks a flag, and joins only an ended thread:

```{.cpp include="examples/threads/alien.cc" snippet="start"}
```

On two shards, this prints `the shards' answers add up to 100`. Seastar
got these two functions from Ceph, whose Crimson project rebuilds
Ceph's storage server on Seastar: Kefu Chai, of Red Hat, added them in
2018, because "it helps when an application is migrating to seastar".

### A worker for code that blocks {index="alien thread!worker"}

The usual use of an alien thread is the other way around: a shard has
work that must not run on it, a library that blocks or a computation
that runs too long, and gives it to a *worker*, a thread of its own,
which runs it and sends back the result. This worker runs one function at
a time, from a queue under a mutex:

```{.cpp include="examples/threads/worker.cc" snippet="worker"}
```

`submit()` is a coroutine, on the shard. Its promise stays in its frame,
and the worker only gets the promise's address: when the function
returns, the worker sends the result to the shard with `run_on()`, and
the promise is fulfilled there, on its own shard:

```{.cpp include="examples/threads/worker.cc" snippet="submit"}
```

`run_on()` only queues its message, and returns at once, so the message
carries the result with it: the worker's variables are gone by the time
the shard runs it, and the first version of this code, which captured
them by reference, read them after they were destroyed. Stopping the
worker waits for what was submitted, without blocking the shard, and
then joins a thread that is about to end:

```{.cpp include="examples/threads/worker.cc" snippet="stop"}
```

The program gives the worker a call that blocks for 300 milliseconds,
standing for a library that waits for the network, and the computation
above, and then calls the same two functions on the shard itself, while
a timer that fires every millisecond counts how often the shard was free
to run it:

```{.cpp include="examples/threads/worker.cc" snippet="use"}
```

```
in the worker: got 42 and 348513 primes, and the timer fired 1180 times
on the shard: got 42 and 348513 primes, and the timer fired 1 times
```

In the worker, the shard ran its timer through the whole second and a
quarter; on the shard, once, after both calls had ended, and the stall
detector reported a stall of more than a second.

A thread inherits the CPU affinity of the thread that starts it, and a
shard's reactor is pinned to its core, so this worker runs on its
shard's core: when both have work, the kernel divides the core between
them. Real workers take care of this, and more. ScyllaDB's, which it
uses to train compression dictionaries for its RPC, stays on the shard's
core, but lowers its priority to the lowest, with `nice(19)`, so that
the reactor gets the core whenever it wants it, and blocks every signal
in the worker, so that signals go to Seastar's threads. Redpanda's,
which runs its Kerberos and OpenSSL code, also blocks every signal,
bounds its queue with a Seastar semaphore, so that the shards can't
submit more than it keeps up with, and wakes the worker through an
eventfd. And Ceph's Crimson runs its classic storage engine,
BlueStore, unchanged, a whole subsystem built on threads, in a pool of
alien threads: a shard's calls go to the pool through a queue bounded by
a semaphore, all calls on one collection go to the same thread, so that
they stay in order, and BlueStore's own threads, which commit
transactions and write its log, call `alien::submit_to()` to reach the
shards.

## Running other programs {index="process"}

[`spawn_process()`]{index="`seastar::spawn_process()`" .main}, from
`<seastar/core/seastar.hh>`, starts another program, and returns a
[`process`]{index="`seastar::process`"}, from
`<seastar/util/process.hh>`, whose standard input, output and error are
Seastar streams ([chapter](#ch:networking)), and whose `wait()` returns
a future of how it ended. This one sorts three words:

```{.cpp include="examples/threads/process.cc" snippet="spawn"}
```

It prints:

```
pear
fig
apple
sort exited with status 0
```

Starting the program is one of the system calls that block, so
`spawn_process()` makes it in the system-call thread. Every process must
be waited for, with `wait()`, even one that was killed, or it stays a
zombie until Seastar's program ends. `process` was experimental until
2026, and older code, ScyllaDB's among it, still names it
`experimental::process`, which `<seastar/util/process.hh>` keeps as a
deprecated alias.

## Exercises

1. Make `worker::submit()` wait, before it queues a function, while the
   worker already has 16 functions waiting, as Redpanda's worker does,
   with a semaphore of the shard's. Submit a hundred functions that each
   sleep 10 milliseconds, all at once, and check that no more than 16 ever
   wait in the queue.

2. Build a pool of four workers, in which every function has a key, and
   functions with the same key always go to the same worker, as Crimson's
   do, so that they run in the order they were submitted. Submit eight
   functions with each of four keys, each sleeping 50 milliseconds, and
   check both the order of each key's functions, and the total time.

3. In `alien.cc`, make the alien thread send 100,000 functions to shard
   0 with `alien::run_on()`, each incrementing a counter, and measure how
   long each takes to send and to run. Compare it with sending a function
   from one shard to another, which [chapter](#ch:talking-between-shards)
   measured.

4. Rewrite `run()` in `deferred.cc` as a coroutine. Why can't it use
   `deferred_stop`? Make it stop its services in the reverse order,
   whichever way it ends, without a Seastar thread.

5. Use `spawn_process()` to run `sh -c 'exit 3'`, and `sh -c 'kill -9
   $$'`, and print how each ended. What happens if the program doesn't
   exist?
