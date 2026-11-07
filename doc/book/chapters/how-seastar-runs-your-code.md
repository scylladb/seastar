# How Seastar runs your code {#ch:how-seastar-runs-your-code}

The previous chapters said that while a coroutine waits, its shard runs
other work. This chapter explains what that other work is, in what order
it runs, when the shard checks the network for new requests, and what
happens when code doesn't give up the CPU for a while. That last question
matters more than it seems. Then we use what we learned to add two
commands to Shoal, `KEYS` and `FLUSHALL`, whose obvious implementations
would freeze the whole server for tens of milliseconds at a time. Finally,
we look at how Seastar talks to the operating system's kernel.

## The reactor

Each shard has a single thread, and on that thread runs a loop that
Seastar calls the *reactor*. Everything that a shard does, it does
because the reactor called it.

### Tasks

The unit of work that the reactor runs is a *task*: a small object that
knows how to run a piece of code. The reactor keeps a queue of tasks that
are ready to run, and runs them one after the other, in the order in which
they became ready. (An application can divide its tasks into several
queues, and tell the reactor how to share the CPU among them; that's the
subject of [chapter](#ch:scheduling-groups). Until then, there is one
queue.)

When a coroutine suspends at a `co_await`, waiting for a future, the
future remembers the coroutine. When the future becomes available, it
doesn't resume the coroutine right away: it puts the coroutine in the
queue, as a task. (The coroutine's frame is itself the task, so this
doesn't even need to allocate memory.) Later, when the reactor reaches
it, running the task resumes the coroutine, from the point where it
stopped, until it suspends again or ends. The same happens to a timer
that expires, to data that arrives on a connection, and to everything
else that a coroutine may wait for: something that happened makes a
future available, and the code that waited for it becomes a task in the
queue.

Running a task is just a function call, and the reactor never interrupts
a task in the middle. A task gives back the CPU only by returning: when
its coroutine suspends at a `co_await`, or ends. This is what
[chapter](#ch:case-for-seastar) called a cooperative scheduler, and the
rest of this chapter is about its consequences.

### The loop

The reactor repeats three steps, over and over:

1. **Run tasks.** It runs the tasks in the queue, one after the other,
   including tasks that the running tasks make ready. It stops when the
   queue is empty, or when it has been running tasks for too long, as we
   will see below.
2. **Poll.** It checks for everything that may have happened in the
   meantime: data that arrived on network connections, new connections,
   disk reads and writes that completed, timers that expired, messages
   from other shards. Each of these makes futures available, and so puts
   more tasks in the queue. Polling also sends the data that output
   streams flushed: this is where the batched flushes of
   [chapter](#ch:coroutines) leave.
3. **Sleep, if there's nothing to do.** If polling found nothing, and the
   queue is empty, there is nothing for the shard to do until something
   happens. The reactor doesn't go to sleep at once: it keeps polling for
   a short while, and only then asks the kernel to put the thread to sleep
   until something happens.

Each of the things that the reactor checks in the second step is a
*poller*, a small object that knows how to check for one kind of event.
An application rarely writes pollers of its own, but it helps to know
that they exist, because it explains when things happen. For example, a
request that arrives while the reactor is running tasks isn't noticed
until the next poll.

### Polling, and sleeping

Why does the reactor poll before it sleeps? Asking the kernel to wake up
a sleeping thread when something happens takes time: the kernel must
notice the event, schedule the thread, and often bring the core back from
a power-saving state. On a busy server, these delays add up to a
noticeable part of each request's latency. Polling, checking for events
without waiting, is much faster, and on a server that is busy anyway, it
costs nothing: there is almost always something to find.

So the reactor polls for a while before it sleeps: by default, for 200
microseconds (2 milliseconds on a virtual machine, where waking a thread
is slower still). The option `--idle-poll-time-us` changes that time,
`--poll-mode` makes the reactor never sleep at all, and
`--overprovisioned` turns polling off, among other things, for machines
where the shards share their cores with other programs: laptops,
containers, and virtual machines whose cores aren't dedicated to them.

One consequence surprises people who meet Seastar for the first time. A
Seastar server that answers a few thousand requests a second may appear,
in `top`, to use its cores fully, because between requests it polls
rather than sleeps. That time is spent looking for work, not doing it, so
`top` can't tell you how loaded a Seastar application is. Its metrics
can: `seastar_reactor_utilization` tells how much of the time each shard
spent doing work. ([Chapter](#ch:observability) is about metrics.)
Exercise 3 shows the difference.

### The task quota

The reactor runs tasks until the queue is empty, but a busy server's
queue may never be empty: every task it runs may make others ready. If
the reactor waited for the queue to empty before polling, a new request,
or a timer that expired, could wait for a very long time. So the reactor
limits how long it runs tasks before it polls again. That limit is the
*task quota*: half a millisecond by default, changed with
`--task-quota-ms`.

How does the reactor know that the quota is used up? A timer fires every
half a millisecond, and when it fires, it raises a flag. After each task,
the reactor checks the flag, and if it is raised, the reactor stops
running tasks, polls, and lowers the flag. A program can check the same
flag by calling `seastar::need_preempt()`, which returns true when the
reactor would like the running task to give the CPU back.

Note what the flag cannot do: it can't stop a task that is running. The
reactor checks it only between tasks, so a task that runs for a long time
without returning keeps the CPU however long it takes. Only the task
itself can check `need_preempt()` and decide to give the CPU back, as we
will see in a moment.

> **How `need_preempt()` can be so cheap.** Code that wants to give up
> the CPU on time must check `need_preempt()` often, so it had better be
> cheap. It is: it reads two numbers from memory and compares them, with
> no system call and no atomic instruction. With the `io_uring` and
> `linux-aio` backends, which the end of this chapter describes, the
> kernel itself raises the flag: Seastar asks the kernel to report
> the timer's expiry by adding an entry to a small ring of events in the
> program's memory, and the two numbers that `need_preempt()` compares
> are that ring's head and tail. A timer that the program set, such as
> the one behind `sleep()`, raises the same flag when it expires. With
> the `epoll` backend, a helper thread waits for the timer and raises the
> flag instead.

## When a coroutine gives up the CPU

A coroutine gives up the CPU when its task returns: when it suspends at a
`co_await`, or ends. The rules for when a `co_await` suspends are the
key to everything in this chapter:

- `co_await` on a future that **isn't available** always suspends: the
  coroutine has nothing to do until the future is available.
- `co_await` on a future that **is available** suspends only if
  `need_preempt()` is true. Then the coroutine goes to the back of the
  queue, as a task that is ready to run, so the reactor can poll and
  other tasks can run. Otherwise, it simply continues, at almost no cost.

The second rule is what makes most Seastar code give up the CPU on time
without any effort: every `co_await` is a place where a coroutine may
yield, and a loop that `co_await`s something on every iteration yields by
itself when its quota runs out. The rest of the list covers the special
cases:

| What the code does | Gives up the CPU |
|:-----------------------------------------------------|:--------------------------|
| `co_await f`, if `f` isn't available | Always |
| `co_await f`, if `f` is available | If `need_preempt()` |
| `co_await coroutine::maybe_yield()` | If `need_preempt()` |
| `co_await seastar::yield()` | Always |
| `co_await coroutine::without_preemption_check(f)` | Never, if `f` is available |
| `f.then(...)` ([chapter](#ch:continuations)) | Never, if `f` is available |
| `f.get()` in a Seastar thread ([chapter](#ch:threads)) | Never, if `f` is available |
| `thread::maybe_yield()` in a Seastar thread | If `need_preempt()` |
| Code without `co_await`, such as a loop | Never |

`coroutine::maybe_yield()` is for loops that have nothing to `co_await`:
it gives up the CPU only when the quota is used up, so it can be called on
every iteration of even a tight loop. `seastar::yield()` gives up the CPU
every time, and is rarely what you want. And
`coroutine::without_preemption_check()` is the opposite of
`maybe_yield()`: it tells `co_await` to use an available future's result
at once, even when the quota is used up, for the rare code where
continuing is more important than letting other tasks run. Rarer still is
`seastar::check_for_io_immediately()`, which raises the `need_preempt()`
flag itself and yields, so that the reactor polls as soon as the running
task returns. The last rows of the table come from styles of Seastar code
that later chapters describe, and are here so that this table is
complete.

One more rule concerns debug builds. In Seastar's `debug` mode,
`need_preempt()` always returns true, so every `co_await` suspends, even
on an available future, and continuations are always scheduled through
the queue. Coroutines are interleaved much more often than in a release
build, which makes debug builds slow, but also very good at finding bugs
that appear only when other code runs at the "wrong" moment.

### An experiment

The program `preemption.cc`, in the book's examples, puts the table to
the test. It runs a *heartbeat*, a coroutine that sleeps for a
millisecond at a time, and measures how late it wakes up:

```{.cpp include="examples/how-seastar-runs-your-code/preemption.cc" snippet="heartbeat"}
```

and, at the same time, a second of computation, in one of several ways
chosen by the `--work` option. `compute()` is a function that calculates
for less than a microsecond, and `compute_coroutine()` is the same
calculation as a coroutine, which never suspends:

```{.cpp include="examples/how-seastar-runs-your-code/preemption.cc" snippet="work"}
```

Here are the results, on one shard, on the laptop of
[chapter](#ch:tutorial-introduction) with its Intel Core Ultra 7 258V:

| `--work` | Calls to `compute()` | Heartbeats | Latest heartbeat |
|---|---|---|---|
| `plain` | 2,255,000 | 1 | 1,000 ms late |
| `maybe_yield` | 2,220,000 | 668 | 0.51 ms late |
| `yield` | 2,182,000 | 999 | 0.19 ms late |
| `coroutine` | 2,180,000 | 668 | 0.51 ms late |
| `without_preemption_check` | 2,185,000 | 1 | 1,000 ms late |
| `block` | 1 | 1 | 1,000 ms late |

With `plain`, the loop never gives up the CPU, so the heartbeat can't run
until the second of computation is over, and then it runs once, a second
late. `without_preemption_check` and `block`, which calls
`std::this_thread::sleep_for()` for a second, have the same effect.

With `maybe_yield` and `coroutine`, the work gives up the CPU whenever
the quota is used up, and the heartbeat is never more than about one task
quota late. Why does it beat only 668 times in a second, and not a
thousand? When the heartbeat's timer expires, the work yields, and the
reactor polls and finds the expired timer. But the work went to the back
of the queue *before* the poll, and the heartbeat, which became ready
during the poll, is behind it. So the work runs for another full quota,
and only then does the heartbeat get its turn: every beat is about half a
millisecond late. This is the price of the task quota: a task that
becomes ready may wait for a quota's worth of work by each task ahead of
it in the queue.

With `yield`, the work gives up the CPU after every computation, so the
heartbeat runs almost as soon as its timer expires. Yielding two million
times a second cost only about 2% of the computation, less than ten
nanoseconds per yield. But a shorter task quota gives the same benefit to
every task, without changing any code (see exercise 2).

## Stalls

When a task runs for a long time without giving up the CPU, nothing else
on its shard runs: no other request is served, no timer fires, no reply
leaves. [Chapter](#ch:case-for-seastar) called this a *stall*. A stall of
a few milliseconds is invisible in throughput, but it adds a few
milliseconds to the latency of every request that arrives during it, and
latency is what a server's users notice.

### The stall detector

Seastar watches for stalls. When a task runs for longer than 25
milliseconds, its *stall detector* writes a message to the log, with a
backtrace of the code that is running. Running `preemption` with
`--work plain` writes:

```
Reactor stalled for 66 ms on shard 0, in scheduling group main. Backtrace: 0x4e093e 0x4920f0 0x49261d 0x4928c9 /lib64/libc.so.6+0x19faf 0x42baad 0x42be23 ...
Reactor stalled for 124 ms on shard 0, in scheduling group main. Backtrace: ...
Reactor stalled for 233 ms on shard 0, in scheduling group main. Backtrace: ...
Reactor stalled for 442 ms on shard 0, in scheduling group main. Backtrace: ...
Reactor stalled for 853 ms on shard 0, in scheduling group main. Backtrace: ...
```

The detector reports a long stall several times, at intervals that
double each time, so the reports show how long the stall lasted, and
where the stalled task was at each point. The backtraces are addresses
in the program; [chapter](#ch:debugging-and-profiling) shows how to
turn them into function names and source lines. The threshold is set by
`--blocked-reactor-notify-ms`, and can be changed while the program runs,
with `engine().update_blocked_reactor_notify_ms()`. To keep the log
readable, the detector writes at most five reports a minute, a number set
by `--blocked-reactor-reports-per-minute`.

Remember the message that every Seastar program of
[chapter](#ch:tutorial-introduction) printed when it started, about a
"perf-based stall detector"? Seastar prefers to watch for stalls with the
kernel's performance-monitoring interface, which can also show where
time was spent inside the kernel, during a system call. If the system
doesn't allow that, Seastar falls back to an ordinary timer, which works
just as well for the code of the program itself.

Now look again at the table: with `--work block`, the heartbeat waited a
whole second, but the stall detector reported nothing. Both ways of
watching for stalls measure the time the shard's thread spends *running*,
and a thread blocked in a system call isn't running at all. So the stall
detector catches loops that run too long, but not the worst kind of
stall, a call that blocks. Blocking shows up only as latency, which is
why it is so important to know what blocks.

### What "never block" really means

Now we can say precisely what Seastar code must avoid. Each of these
stops a shard, and with it every connection, every timer and every
background task on that shard:

- **Blocking system calls.** The C library's `sleep()`, reading a file
  with `read()`,
  `fsync()`, looking up a host name with `getaddrinfo()`, and many more
  may wait for the disk or the network. Seastar has non-blocking
  replacements for all that a server usually needs: `seastar::sleep()`,
  its own file API ([chapter](#ch:files-and-storage)), its own DNS
  resolver. The stall detector doesn't see blocking, so you must know
  what your code calls.
- **Locks for threads.** A `std::mutex` that another thread holds blocks
  the shard until it's released. Within a shard, there are no other
  threads to protect anything from, and shards don't share data, so
  Seastar code doesn't need such locks. When coroutines on the same
  shard need to coordinate, they use Seastar's own semaphores and locks,
  which make a coroutine wait instead of the thread
  ([chapter](#ch:coordination)).
- **Long loops.** A loop over a million items, or a library call that
  works on a huge input, such as compressing a large buffer, runs for as
  long as it takes. Such loops must yield, as we do in Shoal below.
- **Destroying a huge container.** Destroying a container destroys its
  elements one by one, in a loop as long as any other. Clearing a hash
  table of a million keys is a loop of a million steps, as Shoal's
  `FLUSHALL` shows below. Large Seastar applications have learned this
  lesson repeatedly, and each has its own functions to destroy large
  containers gradually.
- **Giant allocations.** A single allocation of many megabytes isn't
  itself a stall, but it is usually filled, copied or destroyed in one
  long loop. And because each shard has its own fixed share of memory,
  divided into pieces by all the allocations before it, a large
  contiguous allocation can fail while the shard still has plenty of free
  memory. Containers that allocate in small pieces avoid both problems
  ([chapter](#ch:memory)).

## One thread, many coroutines

There is a good side to this discipline, and it is a big one. Within a
shard, only one task runs at a time, and a task is never interrupted. So
between two `co_await`s, a coroutine's code runs as if nothing else
existed: no other code can change the data it looks at, and no data race
is possible. Seastar code needs no locks or atomic operations to protect
a shard's own data, and is free of a whole class of bugs that make
multithreaded programming so hard.

The price is that every `co_await` is a point where this guarantee ends.
While a coroutine is suspended, other tasks run, and may change anything
the coroutine looks at. And as the table above shows, *any* `co_await`
may suspend, not only those that wait for something: a `co_await` on a
future that is already available yields when the quota is used up. So a
coroutine can't assume that anything it saw before a `co_await` is still
true after it. Shoal is about to give us an example.

## Shoal, version 2

Version 2 adds two commands to Shoal.[^shoal-v2] `KEYS pattern`
lists the keys that match a pattern, in which `*` matches any string, `?`
any single character, and brackets a set of characters, such as `[abc]`,
`[a-z]` or `[^0-9]`. `FLUSHALL` deletes all the keys. Neither is a
command that a production server should see often, and the Redis
documentation warns against using `KEYS` in production. But both are
useful to people, for example when they explore or clean up a database,
and they are good examples of the work that every server must sometimes
do: work on all the data at once.

[^shoal-v2]: The source of this version is in `shoal/v2/`.

Both commands may take long, so their functions are different from the
other commands': instead of returning a reply, they are coroutines that
write the reply to the client's output stream. The table of commands has
room for both kinds of functions:

```{.cpp include="shoal/v2/commands.cc" snippet="table"}
```

and `execute()` now writes the reply itself:

```{.cpp include="shoal/v2/commands.cc" snippet="execute"}
```

`execute()` returns a future, but it isn't a coroutine: it never needs to
wait for anything itself, so it simply returns the future that `write()`
or the command returned. `handle_connection()` now calls
`co_await execute(db, *req, out)`.

### `KEYS`, first attempt

The obvious way to write `KEYS` is a loop over the hash table, which
collects the matching keys, followed by writing the reply:

```cpp
    for (auto& [key, value] : db) {
        if (glob_match(pattern, key)) {
            found.push_back(key);
        }
    }
```

It works, and the tests pass. But it never gives up the CPU while it
looks at the keys, so its shard stalls for as long as that takes. How
long is that? We loaded Shoal with about 950,000 keys, using
`valkey-benchmark` to set a million random keys out of ten million
possible ones:

```
$ valkey-benchmark -p 6390 -t set -n 1000000 -r 10000000 -P 128 -c 16 -q
```

Then, while one client ran `KEYS 'nomatch*'` (a pattern that matches
nothing, so the reply is empty) ten times a second, another client sent
Shoal `PING`s, one at a time, with `valkey-benchmark -t ping_inline -c 1`.
Shoal usually answers a `PING` within 7 microseconds, but while `KEYS`
ran, the latest `PING` waited 66 milliseconds, and Shoal's log showed
why:

```
Reactor stalled for 67 ms on shard 0, in scheduling group main. Backtrace: ...
```

Valkey's `KEYS` is no better: it runs to the end without serving anyone
else. It looks at the keys faster than Shoal does, but the latest `PING`
still waited 21 milliseconds. On a larger database, every `KEYS`
freezes the server for longer.

### Second attempt

The cure for a long loop is to yield in it. Let's add
`co_await coroutine::maybe_yield()` to every iteration:

```cpp
    for (auto it = db.begin(); it != db.end(); ++it) {
        if (glob_match(pattern, it->first)) {
            found.push_back(it->first);
        }
        co_await coroutine::maybe_yield();
    }
```

This version doesn't stall, but it is wrong. When `maybe_yield()` yields,
other clients' commands run, and they may change the hash table:

- `DEL` may delete the very key that `it` points to. Then `++it` reads
  memory that was freed.
- `SET` may add a key, and when the table grows, a `std::unordered_map`
  *rehashes*: it moves all its keys to a new, larger array of buckets.
  Then `it` continues in an order that has nothing to do with the order
  so far, and may return keys twice, or skip some.

This is the price of "any `co_await` may suspend" from the previous
section, in its most common form: an iterator kept across a `co_await`.
Shoal's tests include one that runs `KEYS` many times while another
client adds and deletes keys, and checks that every key that exists all
along is listed exactly once. With this version of `KEYS`, the test
failed in two of three runs.

### Third attempt

The fix is to keep nothing across a `co_await` that other code can
invalidate. A `std::unordered_map` lets us look at the keys in one
*bucket* at a time, by the bucket's number. So `KEYS` looks at one bucket,
and may yield before the next, keeping only the next bucket's number:

```{.cpp include="shoal/v2/commands.cc" snippet="keys"}
```

Every key that exists all along is listed exactly once, but keys that
other clients add or delete while `KEYS` runs may or may not be listed.
So the list may not match the keys of any single moment, as the list of
Redis's `KEYS`, which never yields, does. For a command that people use
to explore a database, this weaker promise is good enough, and it is the
promise that Redis itself makes for its `SCAN` command, which lists the
keys a few at a time. The one change that breaks the numbering of buckets
is a rehash, which changes the number of buckets, and then `KEYS` must start
over. That's rare: the table grows only when the number of keys passes
the number of buckets, and then the number of buckets doubles. In the
test, which adds a hundred thousand keys while `KEYS` runs again and
again, `KEYS` started over once in most runs.

Note the other details that keep this coroutine from stalling:

- The matching keys go into a `std::deque`, which allocates memory in
  small pieces, rather than into a `std::vector`, which would allocate one
  contiguous array for them all: 32 megabytes for a million keys.
- The reply is written one key at a time, and each key is removed from
  the `deque` after it is written, so that the `deque` is destroyed
  gradually. Each `co_await out.write(...)` usually gets an available
  future, since `write()` only copies the data into the stream's buffer,
  so this loop yields when the quota is used up, without any
  `maybe_yield()`.
- To start over, `KEYS` must empty `found`, which may hold a million
  keys. It does so with `clear_gently()`, which `FLUSHALL` needs too.

### `FLUSHALL`

The obvious `FLUSHALL` calls `db.clear()`, which destroys the keys and
the values one by one, in a single loop. With 950,000 keys, Shoal's log
showed a stall of 69 milliseconds. So Shoal destroys them gradually
instead, with a small function that destroys a container's elements one
at a time, yielding when the quota is used up:

```{.cpp include="shoal/v2/commands.cc" snippet="clear-gently"}
```

But `FLUSHALL` must not leave the store half empty while it yields:
other clients would see some keys deleted and others not yet. So it first
moves all the data out of the store, which is quick, and leaves the store
empty, and only then destroys it:

```{.cpp include="shoal/v2/commands.cc" snippet="flushall"}
```

Redis accepts an optional argument, `ASYNC` or `SYNC`, which says whether
to free the memory in a background thread. Shoal accepts both, and
behaves the same with either.

### The results

Here is the latest `PING` of each experiment, while `KEYS` ran ten times
a second, and while `FLUSHALL` deleted all the keys, with about 950,000
keys, and with each server on one core. Each number is the worse of two
runs:

| Server | Latest `PING`, during `KEYS` | Latest `PING`, during `FLUSHALL` |
|---|---|---|
| `valkey-server` | 21 ms | 45 ms (`SYNC`), 3 ms (`ASYNC`) |
| Shoal, without yielding | 66 ms | 63 ms |
| Shoal version 2 | 6 ms | 4 ms |

Even when the server does nothing else, the latest of 400,000 `PING`s
takes 1 to 3.4 milliseconds, because the client and the server share the
laptop with everything else that runs on it. So version 2's
`FLUSHALL` adds nothing measurable to the latency of other clients.

Its `KEYS` adds a little, and not because of a stall: even with the
threshold at 2 milliseconds, the stall detector reported nothing. Most
`PING`s were still answered within a millisecond and a half, but about one
in a thousand took 3 to 4 milliseconds. That is the price of the task
quota that we saw in the experiment: every step of a `PING`, from
noticing that it arrived to sending the reply, waits for the reactor to
poll, and while `KEYS` has work to do, the reactor polls only once per
quota. With `--task-quota-ms 0.1`, fewer than six `PING`s in 400,000
took more than a millisecond and a half. A better way to protect the
latency of short requests from long ones is to put them in separate
*scheduling groups*, and tell Seastar how to share the CPU between them,
as [chapter](#ch:scheduling-groups) shows.

### A stall we didn't write

The stall detector found one more stall in Shoal, and not in a command
we would have suspected. In two of three runs, while `valkey-benchmark`
loaded the keys, it reported a stall of about 70 milliseconds in `SET`.
The backtraces showed where: deep inside
`std::unordered_map`, which rehashes all its keys at once whenever it
grows. The more keys, the longer each rehash takes; with a million keys,
it takes tens of milliseconds. Redis and Valkey avoid this with hash
tables of their own, which move the keys to the larger table a few at a
time, as part of the commands that follow. Shoal will live with the
stall for now; [chapter](#ch:stalls) returns to it, and to the general
problem of stalls in code you didn't write.

## How the reactor talks to the kernel

When the reactor polls, it asks the kernel what happened, and when it has
nothing to do, it asks the kernel to put the thread to sleep until
something happens. Linux offers several interfaces for both, and Seastar
can use any of them, through a *reactor backend*. The first line that a
Seastar program logs tells which one it uses, and `--reactor-backend`
chooses another:

- **`epoll`**, the oldest, uses the interface that event-driven servers
  have used since 2002. `epoll` tells the reactor which connections are
  ready to be read or written, and the reactor then reads and writes them
  with ordinary non-blocking system calls. Disk I/O goes through Linux's
  asynchronous I/O interface, *Linux AIO*. A helper thread waits for the
  task quota's timer, to raise the `need_preempt()` flag.
- **`linux-aio`** does everything through Linux AIO, which since Linux
  4.18 can also report that a connection is ready. It collects network
  and disk events with the same system call, and needs no helper thread.
  Each shard reserves room for many AIO requests, and the system-wide
  limit on them, `/proc/sys/fs/aio-max-nr`, may need raising on machines
  with many cores.
- **`io_uring`** uses Linux's newest asynchronous interface, which
  handles both network and disk I/O. It is the default when the kernel
  supports it, and Seastar was built with the `liburing` library, which
  Seastar's `install-dependencies.sh` installs.
- **`asymmetric_io_uring`** also uses io_uring, but moves the system
  calls, and the kernel's work behind them, to dedicated cores, which
  `--async-workers-cpuset` names; the shards only queue their requests,
  and collect the results. Several shards share each of these cores. It
  refuses to start without that option: this is the backend of exercise 4
  of [chapter](#ch:getting-started).

Whatever the backend, reading from a connection doesn't always wait for
the kernel to say that data has arrived. When a read fills the whole
buffer it was given, there is probably more data waiting, so the
reactor's next read on that connection tries the system call at once.
Seastar calls this *speculation*. The asymmetric backend never
speculates: all of its I/O goes to the dedicated cores.

Shoal version 2, on one core and measured as in
[chapter](#ch:coroutines), gave these results with each backend:

| Backend | `SET` per second | `GET` per second |
|---|---|---|
| `epoll` | 3,400,000 | 4,000,000 |
| `linux-aio` | 3,470,000 | 4,100,000 |
| `io_uring` | 3,550,000 | 4,210,000 |
| `asymmetric_io_uring`, with a second core | 3,800,000 | 4,570,000 |

A second run agreed within 2%. The three ordinary backends are within 5%
of each other, so for a server like Shoal, the choice of backend matters
less than one might expect. The asymmetric backend did 7% to 9% more on
one shard, but it used a second core to do it, and that core could
instead have run a second shard. The asymmetric backend is meant for
shards that are busy with computation rather than I/O, so that moving
the kernel's work elsewhere leaves them more time for their own. [Chapter](#ch:deploying-and-tuning)
discusses how to choose.

Disk I/O deserves a word too. Seastar reads and writes files with
*direct I/O*, which bypasses the kernel's page cache. With Linux AIO,
this is essential: only direct I/O is truly asynchronous there, and an
ordinary read of a file may block the shard inside the system call that
was supposed to start it in the background. Direct I/O also leaves the
caching to the application, which knows best what is worth keeping in
memory. [Chapter](#ch:files-and-storage) explains how to work with it.

## Exercises

1. Build Shoal with the second attempt at `KEYS`, the one that keeps an
   iterator across `maybe_yield()`, and run its tests a few times with
   `./run -k keys_while_changing`. How does the test fail? Can you tell,
   from the failures, which of the two problems described above happened?

2. Run `preemption --work maybe_yield` with `--task-quota-ms 0.1`, and
   with `--task-quota-ms 5`. How late is the latest heartbeat, and how
   many beats are there? Explain the numbers. What would a very short task
   quota cost a server that is busy with requests?

3. Start Shoal on one core (`--cpuset 0`), and watch its CPU use in `top`
   while it is idle, and while
   `valkey-cli -p 6390 -r 5000 -i 0.001 PING > /dev/null` sends it a
   `PING` every millisecond. Then repeat both with
   `--idle-poll-time-us 0`. How much CPU time does each `PING` cost, with
   and without idle polling? What is the rest of that time spent on?

4. Load Shoal with 4 million keys (`-n 4000000 -r 40000000`), running it
   with `-m4G --blocked-reactor-notify-ms 2`. How long are the stalls
   when the hash table grows, and how do they grow with the number of
   keys? How would you build a hash table that grows without stalling?

5. Redis's `FLUSHALL ASYNC` replies at once, and frees the memory in the
   background. Make Shoal's `FLUSHALL ASYNC` do the same: reply, and then
   destroy the old data in a coroutine that runs in the background. What
   must you be careful about? (Hint: who owns `old` now?)
   [Chapter](#ch:concurrency-within-a-shard) discusses background work.
