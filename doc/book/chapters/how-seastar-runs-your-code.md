# How Seastar runs your code {#ch:how-seastar-runs-your-code}

The previous chapters said that while a coroutine waits, its shard runs
other work. This chapter explains what that other work is, in what order
it runs, when the shard checks the network for new requests, and what
happens when code doesn't give up the CPU for a while. That last question
matters more than it seems. Then we use what we learned to add two
commands to Shoal, `KEYS` and `FLUSHALL`, whose obvious implementations
would freeze the whole server for tens of milliseconds at a time. Finally,
we look at how Seastar talks to the operating system's kernel.

## The reactor {index="reactor"}

Each shard has a single thread, and on that thread runs a loop that
Seastar calls the [*reactor*]{.index .main}. Everything that a shard does, it does
because the reactor called it.

### Tasks

The unit of work that the reactor runs is a [*task*]{index="task" .main}: a small object that
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

### The loop {index="reactor!main loop"}

The reactor repeats three steps, over and over
([Figure](#fig:reactor-loop)):

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

![The reactor's loop. It runs the ready tasks until the queue is empty or
the task quota is used up, and then polls for everything that may have
happened, which makes more tasks ready. Only when polling finds nothing
for a while does the thread go to sleep.](figures/reactor-loop.svg){#fig:reactor-loop}

Each of the things that the reactor checks in the second step is a
[*poller*]{.index .main}, a small object that knows how to check for one kind of event.
An application rarely writes pollers of its own, but it helps to know
that they exist, because it explains when things happen. For example, a
request that arrives while the reactor is running tasks isn't noticed
until the next poll.

### Polling, and sleeping {index="polling"}

Why does the reactor poll before it sleeps? Asking the kernel to wake up
a sleeping thread when something happens takes time: the kernel must
notice the event, schedule the thread, and often bring the core back from
a power-saving state. On a busy server, these delays add up to a
noticeable part of each request's latency. Polling, checking for events
without waiting, is much faster, and on a server that is busy anyway, it
costs nothing: there is almost always something to find.

So the reactor polls for a while before it sleeps: by default, for 200
microseconds (2 milliseconds on a virtual machine, where waking a thread
is slower still). The option [`--idle-poll-time-us`]{index="command-line options!`--idle-poll-time-us`" .main} changes that time,
[`--poll-mode`]{index="command-line options!`--poll-mode`" .main} makes the reactor never sleep at all, and
[`--overprovisioned`]{index="command-line options!`--overprovisioned`" .main} turns polling off, among other things, for machines
where the shards share their cores with other programs: laptops,
containers, and virtual machines whose cores aren't dedicated to them.
It's also the option for tests, which often run several servers, and
their clients, on one machine: servers that poll would fight over its
cores, and slow down each other's tests. ScyllaDB's test framework starts
every server it tests with `--overprovisioned`, and so does Shoal's, from
version 6 on.

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
[*task quota*]{.index .main}: half a millisecond by default, changed with
[`--task-quota-ms`]{index="command-line options!`--task-quota-ms`"}.

Why half a millisecond? A shorter quota makes the reactor poll more
often, so a new request, or an expired timer, waits less for its turn.
But each poll, and each time a task gives up the CPU, costs a little, so
a shorter quota leaves less of the CPU for the work itself. Half a
millisecond is a compromise that came from experience. Seastar's first
quota measured in time, in 2015, was 2 milliseconds, and in 2018 it was
lowered to half a millisecond, which ScyllaDB had been using "for a long
time". Exercise 2 at the end of this chapter measures both sides of the
compromise.

How does the reactor know that the quota is used up? A timer fires every
half a millisecond, and when it fires, it raises a flag. After each task,
the reactor checks the flag, and if it is raised, the reactor stops
running tasks, polls, and lowers the flag. A program can check the same
flag by calling `seastar::need_preempt()`, from
`<seastar/core/preempt.hh>`, which returns true when the
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
> are that ring's head and tail. The same ring also reports a second
> timer of the kernel's, which the reactor sets for the earliest of the
> program's own `std::chrono::steady_clock` timers, such as the one
> behind `seastar::sleep()`. So when such a timer is due, the flag goes
> up too, and the running tasks yield early, letting the timer fire on
> time rather than up to half a millisecond late. That is one wakeup for
> the earliest expiration time, however many timers share it, and
> timers on the cheaper `lowres_clock`, which [chapter](#ch:time)
> describes, never raise the flag. With the `epoll` backend, a helper
> thread waits for both of the kernel's timers and raises the flag
> instead.

## When a coroutine gives up the CPU {index="preemption"}

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
without any effort: every `co_await` is a
[*preemption point*]{index="preemption point" .main}, a place where a
coroutine may yield, and a loop that `co_await`s something on every iteration yields by
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

`coroutine::maybe_yield()`, from `<seastar/coroutine/maybe_yield.hh>`,
is for loops that have nothing to `co_await`:
it gives up the CPU only when the quota is used up, so it can be called on
every iteration of even a tight loop. For example, this coroutine counts
the lines that contain a word:

```{.cpp include="examples/how-seastar-runs-your-code/count-lines.cc" snippet="count-lines"}
```

Given two million lines, it counts for about 13 milliseconds. The example
runs another task at the same time, which counts its turns, and that
task gets the CPU 22 times during the count, about once per quota.
Without the `co_await coroutine::maybe_yield()`, the other task gets its
turn only once the count is over. Notice that the coroutine takes its
lines by value, so it owns them. While it has given up the CPU, other
tasks run, and if one of them could change the vector, the loop's
iterator could be left pointing at freed memory; we will see this happen
later in this chapter, with Shoal's `KEYS` command.

`seastar::yield()`, from `<seastar/util/later.hh>`, gives up the CPU
every time, and is rarely what you want. And
`coroutine::without_preemption_check()` is the opposite of
`coroutine::maybe_yield()`: it tells `co_await` to use an available future's result
immediately, even when the quota is used up, for the rare code where
continuing is more important than letting other tasks run. Rarer still is
`seastar::check_for_io_immediately()`, from `<seastar/util/later.hh>`,
which raises the `need_preempt()`
flag itself and yields, so that the reactor polls as soon as the running
task returns. The last rows of the table come from styles of Seastar code
that later chapters describe, and are here so that this table is
complete.

One more rule concerns debug builds. In Seastar's `debug` mode,
`need_preempt()` always returns true, so every `co_await` suspends, even
on an available future, and continuations are always scheduled through
the queue. And the queue isn't kept in order: each task that becomes
ready swaps places with a random task already in the queue, so tasks
don't run in the order in which they became ready. Coroutines are
interleaved much more often than in a release build, and in an order
that changes from run to run. This makes debug builds slow, but also
very good at finding bugs that appear only when other code runs at the
"wrong" moment, or that silently depend on an order that Seastar doesn't
promise.

### An experiment

The program `preemption.cc`, in the book's examples, puts the table to
the test. It runs a [*heartbeat*]{index="heartbeat" .main}, a coroutine that sleeps for a
millisecond at a time, and measures how late it wakes up:

```{.cpp include="examples/how-seastar-runs-your-code/preemption.cc" snippet="heartbeat"}
```

and, at the same time, a second of computation, in one of several ways
chosen by the `--work` option. The computation is made of small pieces:
`compute()` is a function that calculates for less than a microsecond,
and `compute_coroutine()` is the same calculation as a coroutine, which
never suspends:

```{.cpp include="examples/how-seastar-runs-your-code/preemption.cc" snippet="compute"}
```

`work()` calls one of them again and again until the second is over,
giving up the CPU in the way that `--work` chose:

```{.cpp include="examples/how-seastar-runs-your-code/preemption.cc" snippet="work"}
```

Here are the results, on one shard, on the laptop of
[chapter](#ch:tutorial-introduction) with its Intel Core Ultra 7 258V,
in three runs:

| `--work` | Calls to `compute()` | Heartbeats | Most delayed heartbeat |
|---|---|---|---|
| `plain` | 2,242,000–2,257,000 | 1 | 1 second late |
| `maybe_yield` | 2,233,000–2,268,000 | 668 | 0.51–0.52 ms late |
| `yield` | 2,156,000–2,206,000 | 999 | 0.01–0.10 ms late |
| `coroutine` | 2,195,000–2,198,000 | 668 | 0.50–0.52 ms late |
| `without_preemption_check` | 2,191,000–2,199,000 | 1 | 1 second late |
| `block` | 1 | 1 | 1 second late |

With `plain`, the loop never gives up the CPU, so the heartbeat can't run
until the second of computation is over, and then it runs once, a second
late. `without_preemption_check` and `block`, which calls
`std::this_thread::sleep_for()` for a second, have the same effect.

With `maybe_yield` and `coroutine`, the work gives up the CPU whenever
the quota is used up, and the heartbeat is never more than about one task
quota late. Why does it beat only 668 times in a second, and not a
thousand ([Figure](#fig:heartbeat))? When the heartbeat's timer expires, the work yields, and the
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

![The first milliseconds of `preemption.cc`, for three kinds of `--work`.
Light bars are the work, a tall dark mark is a heartbeat, and a
triangle is its timer expiring. With `plain`, the heartbeat waits for the whole
second. With `maybe_yield`, the work yields when the timer expires, but
it is ahead of the heartbeat in the queue and runs for one more quota, so
every beat is half a millisecond late, and beats come every 1.5
milliseconds. With `yield`, the heartbeat runs almost as soon as its
timer expires.](figures/heartbeat.svg){#fig:heartbeat}

## Stalls {index="stall"}

When a task runs for a long time without giving up the CPU, nothing else
on its shard runs: no other request is served, no timer fires, no reply
leaves. We call this situation a [*stall*]{.index .main}. A stall of
a few milliseconds is invisible in throughput, but it adds a few
milliseconds to the latency of every request that arrives during it, and
the server's users notice this latency.

> **Tail latency.** A server doesn't answer every request in the same
> time: most requests are answered quickly, and a few slowly. So its
> latency is described by [*percentiles*]{index="latency!percentiles" .main}.
> The median, or 50th percentile (p50), is the latency that half of the
> requests beat; the 99th percentile (p99) is the latency that 99% of
> them beat; and so on, to p99.9 and beyond. The slowest requests, those
> beyond p99 or so, are the distribution's
> [*tail*]{index="tail latency" .main}. Stalls hurt the tail, and hardly
> anything else. Suppose that a server answers each request in 0.1
> milliseconds, but stalls for 25 milliseconds once a second. The 2.5% of
> the requests that arrive during a stall wait until it ends: up to 25
> milliseconds for a request that arrives at its start, and hardly at all
> for one that arrives at its end. The slowest 1% of all the requests are
> the 40% of these that arrived first, and they wait between 15 and 25
> milliseconds. So p99 is about 15 milliseconds, 150 times the usual
> latency, while p50 doesn't change, and the average grows by only 0.3
> milliseconds.
>
> The tail matters more than its size suggests, because a user's request,
> such as loading a web page, often waits for many requests to servers,
> and is as slow as the slowest of them. Jeffrey Dean and Luiz André
> Barroso, in "The Tail at Scale" (*Communications of the ACM*, February
> 2013), give an example: if one request in a hundred to a server is
> slow, then a user's request that waits for a hundred such servers is
> slow 63% of the time. This is why a server's latency is judged by its
> p99 or p99.9 rather than by its average, and why a Seastar server must
> avoid stalls even if they delay only one request in a hundred.

### The stall detector

Because stalls cause high latency, especially high tail latency, Seastar
helps you find them, with its built-in
[*stall detector*]{.index .main}. When a task runs for longer than 25
milliseconds, the stall detector writes a message to the log, with a
backtrace of the code that is running, to help you find the source of
the stall so you can fix it. Running `preemption` with `--work plain`
writes:

```
Reactor stalled for 66 ms on shard 0, in scheduling group main. Backtrace: 0x4e093e 0x4920f0 0x49261d 0x4928c9 /lib64/libc.so.6+0x19faf 0x42baad 0x42be23 ...
Reactor stalled for 124 ms on shard 0, in scheduling group main. Backtrace: ...
Reactor stalled for 233 ms on shard 0, in scheduling group main. Backtrace: ...
Reactor stalled for 442 ms on shard 0, in scheduling group main. Backtrace: ...
Reactor stalled for 853 ms on shard 0, in scheduling group main. Backtrace: ...
```

These five reports are all about one stall: the second of work, which
ran in a single task. The detector reports a long stall several times,
at intervals that double each time, so the reports show where the
stalled task was at each point, and how long the stall had lasted so
far. So 853 milliseconds isn't the stall's length, only how long it had
lasted at the last report: it went on until the work ended, after about
a second, and the stall ended before the next report was due, at about
1.65 seconds. The doubling lasts only as long as the stall; the next
stall is reported at the usual intervals again. The backtraces are
addresses in the program; [chapter](#ch:debugging-and-profiling) shows
how to turn them into function names and source lines.

Why was the first report written after 66 milliseconds, and not 25? The
detector doesn't watch the running task all the time. It checks at
intervals of 25 milliseconds of CPU time plus 30% to spare, 32.5
milliseconds in all, and reports a stall only if no task finished during
a whole interval. Here, another task finished during the first
interval, just before the work began, so the first interval with no
finished task ended at the second check, at about 65 milliseconds. The following reports came after intervals of
57.5, 107.5, 207.5 and 407.5 milliseconds: 50, 100, 200 and 400, each
with the same 7.5 milliseconds to spare.

The 25 millisecond threshold can be changed with
[`--blocked-reactor-notify-ms`]{index="command-line options!`--blocked-reactor-notify-ms`"}. To keep the log
readable, the detector writes at most five reports a minute, a number set
by `--blocked-reactor-reports-per-minute`. The limit counts reports, not
stalls, so a single long stall, like the one above, can use up the whole
minute's reports. When the minute is over, the count starts again, and
the detector writes how many reports it left out, in a line such as
`Rate-limit: suppressed 3 backtraces on shard 0`.

Remember the message that every Seastar program of
[chapter](#ch:tutorial-introduction) printed when it started, about a
"perf-based stall detector"? Seastar prefers to watch for stalls with the
kernel's performance-monitoring interface, because then the report of a
stall inside a system call shows where in the kernel the time went, too.
This needs `/proc/sys/kernel/perf_event_paranoid` to be 1 or less (most
distributions set it to 2), or root privileges (precisely, the
`CAP_PERFMON` capability). Otherwise, Seastar falls back to an ordinary
timer, which reports the same stalls, without the kernel's part of the
backtrace.

Either way, the detector measures the CPU time that the shard's thread
uses, so it can't see a thread that sleeps in a system call. With
`--work block`, the heartbeat waited a whole second, but the detector
reported nothing: the thread slept for that whole second, and used no
CPU. So the stall detector catches code that runs too long, but not a
call that blocks. Such calls are easy to avoid, though: Seastar code
shouldn't make blocking system calls at all, and the next section lists
the usual ones and their replacements. Code that must use a library that
blocks can run it in a separate operating-system thread, outside the
reactor, as [chapter](#ch:threads) shows.

### What "never block" really means {index="blocking"}

Now we can say precisely what Seastar code must avoid. Each of these
stops a shard, and with it every connection, every timer and every
background task on that shard:

- **Blocking system calls.** The C library's `sleep()`, reading a file
  with `read()`,
  `fsync()`, looking up a host name with `getaddrinfo()`, and many more
  may wait for the disk or the network. Seastar has non-blocking
  replacements for all that a server usually needs: `seastar::sleep()`,
  its own file API ([chapter](#ch:files-and-storage)), its own [DNS
  resolver]{index="DNS resolver"}. The stall detector doesn't see blocking, so you must know
  what your code calls.
- **Page faults that wait for the disk.** Even a memory access can
  block. If a file is mapped into memory with `mmap()`, reading a part of
  it that isn't in memory makes the kernel read it from the disk, and
  the whole thread, so the whole shard, waits, as in a `read()` call,
  but with no call in the code to warn you. Memory that the kernel has
  swapped out to disk is the same. So Seastar code reads files with
  Seastar's file API rather than mapping them, and
  [`--lock-memory`]{index="command-line options!`--lock-memory`"} tells
  Seastar to lock its memory, so that the kernel never swaps it out.
  Like other blocking, these waits are invisible to the stall detector.
- **Locks for threads.** A [`std::mutex`]{index="`std::mutex`"} that another thread holds blocks
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

## One thread, many coroutines {index="concurrency!within a shard"}

There is a good side to this discipline, and it is a big one. Within a
shard, only one task runs at a time, and a task is never interrupted. So
between two `co_await`s, a coroutine's code runs as if nothing else
existed: no other code can change the data it looks at, and no [data race]{index="data race"}
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

## Shoal, version 2 {index="Shoal!version 2"}

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
write the reply to the client's output stream. So each entry in the
table of commands now has room for a function of either kind, `run` for
a quick command and `run_long` for one that may take long:

```{.cpp include="shoal/v2/commands.cc" snippet="table"}
```

and `execute()` now writes the reply itself:

```{.cpp include="shoal/v2/commands.cc" snippet="execute"}
```

`execute()` returns a future, but it isn't a coroutine: it never needs to
wait for anything itself, so it simply returns the future that `write()`
or the command returned. `handle_connection()` now calls
`co_await execute(db, *req, out)`. (`lowercase()` is version 1's loop
that converted the command's name to lowercase, moved into a function,
because `FLUSHALL` needs it too, for its `ASYNC` and `SYNC` arguments.)

### `KEYS`, first attempt {index="Redis commands!`KEYS`"}

The obvious way to write `KEYS` is a loop over the hash table, which
collects the matching keys, followed by writing the reply:

```cpp
    for (auto& [key, value] : db) {
        if (glob_match(pattern, key)) {
            found.push_back(key);
        }
    }
```

(`glob_match()`, not shown, checks a key against a Redis pattern, in which
`*` matches any string, `?` any one character, and `[...]` any of the
characters listed.)

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
ran, the slowest `PING` waited 66 milliseconds, and Shoal's log showed
why:

```
Reactor stalled for 67 ms on shard 0, in scheduling group main. Backtrace: ...
```

Valkey's `KEYS` is no better: it also runs to the end without serving anyone
else. It looks at the keys faster than Shoal does, because Valkey's own
hash table was designed to be walked quickly: it walks its array of
buckets in the order they lie in memory, and asks the CPU to fetch the
next buckets' keys into its cache before it needs them. Walking a
`std::unordered_map` jumps from one place in memory to an unrelated
other, so every key costs a cache miss, and checking the keys against
the pattern costs almost nothing in comparison. [Chapter](#ch:memory)
returns to this. But even Valkey's slowest `PING` waited 21
milliseconds. On a larger database, every `KEYS`
freezes the server for longer.

### Second attempt {index="Redis commands!`KEYS`"}

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

This version doesn't stall, but it is wrong. When
`coroutine::maybe_yield()` yields,
other clients' commands run, and they may change the hash table:

- `DEL` may delete the very key that `it` points to. Then `++it` reads
  memory that was freed.
- `SET` may add a key, and when the table grows, a `std::unordered_map`
  [*rehashes*]{index="`std::unordered_map`!rehashing"}: it moves all its keys to a new, larger array of buckets.
  Then `it` continues in an order that has nothing to do with the order
  so far, and may return keys twice, or skip some.

This is the price of "any `co_await` may suspend" from the previous
section, in its most common form: an iterator kept across a `co_await`.
Shoal's tests include one that runs `KEYS` many times while another
client adds and deletes keys, and checks that every key that exists all
along is listed exactly once. With this version of `KEYS`, the test
failed in two of three runs.

### Third attempt {index="Redis commands!`KEYS`"}

The fix is to keep nothing across a `co_await` that other code can
[invalidate]{index="iterator invalidation"}. A `std::unordered_map` lets us look at the keys in one
[*bucket*]{index="`std::unordered_map`!buckets"} at a time, by the bucket's number. So `KEYS` looks at one bucket,
and may yield before the next, keeping only the next bucket's number:

```{.cpp include="shoal/v2/commands.cc" snippet="keys"}
```

(`array_header_reply()`, a new reply function, writes the line, such as
`*3`, that starts an array of three strings.)

Why does a bucket's number work where an iterator didn't? An iterator
points to one key's place in memory, so if `DEL` deletes that key, the
iterator points to freed memory, and nothing can tell. A bucket's number
points to nothing: whatever other clients delete, bucket `b` is still
there, and `db.begin(b)` finds the keys that are in it now. Adding a key
is harmless too, as long as the table doesn't rehash. So a rehash is the
only change that `KEYS` must check for.

Every key that exists all along is listed exactly once, but keys that
other clients add or delete while `KEYS` runs may or may not be listed.
So the list may not match the keys of any single moment, as the list of
Redis's `KEYS`, which never yields, does. For a command that people use
to explore a database, this weaker promise is good enough, and it is the
promise that Redis itself makes for its `SCAN` command, which lists the
keys a few at a time. A rehash, though, changes the number of buckets and
moves every key, and then `KEYS` must start over. That's rare: the table
grows only when the number of keys passes the number of buckets, and
then the number of buckets doubles. In the test, which adds a hundred
thousand keys while `KEYS` runs again and again, `KEYS` started over once
in most runs. Redis's `SCAN` doesn't need to start over: its tables have
a power-of-two number of buckets, and it visits the buckets in an order
(their numbers with the bits reversed) that stays valid when a table
doubles. That can't work with `std::unordered_map`, whose numbers of
buckets are prime numbers in libstdc++, so that a key's new bucket has
nothing to do with its old one. [Chapter](#ch:memory) returns to this.

Note the other details that keep this coroutine from stalling:

- The matching keys go into a `std::deque`, which allocates memory in
  small pieces, rather than into a `std::vector`, which would allocate one
  contiguous array for them all: 32 megabytes for a million keys.
- The reply is written one key at a time, and each key is removed from
  the `deque` after it is written, so that the `deque` is destroyed
  gradually. Each `co_await out.write(...)` usually gets an available
  future, since `write()` only copies the data into the stream's buffer,
  so this loop yields when the quota is used up, without any
  `coroutine::maybe_yield()`.
- To start over, `KEYS` must empty `found`, which may hold a million
  keys. Destroying them all at once would be a long loop too, so it
  empties `found` gradually, with `clear_gently()`.

[`clear_gently()`]{index="`clear_gently()` (Shoal)" .main} is a small
function of Shoal's, which destroys a container's elements one at a time,
yielding when the quota is used up. Seastar has no such function, so
every large Seastar application writes its own; ScyllaDB's is also called
`clear_gently()`, and Redpanda's is `ssx::async_clear()`. Shoal's is:

```{.cpp include="shoal/v2/commands.cc" snippet="clear-gently"}
```

### `FLUSHALL` {index="Redis commands!`FLUSHALL`"}

The obvious `FLUSHALL` calls `db.clear()`, which destroys the keys and
the values one by one, in a single loop. With 950,000 keys, Shoal's log
showed a stall of 69 milliseconds. So Shoal destroys them gradually
instead, with `clear_gently()`.

But `FLUSHALL` shouldn't leave the store half empty while it yields.
Redis runs one command at a time, so its clients never see a `FLUSHALL`
half done: for them, all the keys disappear at one moment. If Shoal's
`FLUSHALL` deleted the keys while other clients' commands ran, they
could see some keys deleted and others not yet, and a key that a client
had just set might be deleted after its `SET` succeeded. So `FLUSHALL`
first moves all the data out of the store, which is quick, and leaves the
store empty, and only then destroys it:

```{.cpp include="shoal/v2/commands.cc" snippet="flushall"}
```

The syntax error's branch uses a short form: `co_return co_await f`
waits for `f`, and returns its result. Here, `f` is a `future<>`, whose
result is nothing, so this is simply a shorter way to write
`co_await f;` followed by `co_return;`.

Redis accepts an optional argument, `ASYNC` or `SYNC`, which says whether
to free the memory in a background thread. With `ASYNC`, Redis replies at
once, and frees the memory later; with `SYNC`, it frees the memory before
it replies, and stalls while it does. Shoal accepts both, and for now
behaves the same with either: it replies only after all the old data is
destroyed, like Redis's `SYNC`, but without stalling.
[Chapter](#ch:concurrency-within-a-shard) makes `ASYNC` reply at once,
and destroy the data in the background, and
[chapter](#ch:clean-shutdown) makes a clean shutdown stop that
background work.

### The results

Here is the slowest `PING` of each experiment, while `KEYS` ran ten times
a second, and while `FLUSHALL` deleted all the keys, with about 950,000
keys, and with each server on one core. Each number is the worse of two
runs:

| Server | Slowest `PING`, during `KEYS` | Slowest `PING`, during `FLUSHALL` |
|---|---|---|
| `valkey-server` | 21 ms | 45 ms (`SYNC`), 3 ms (`ASYNC`) |
| Shoal, without yielding | 66 ms | 63 ms |
| Shoal version 2 | 6 ms | 4 ms |

Even when the server does nothing else, the slowest of 400,000 `PING`s
takes 1 to 3.4 milliseconds, because the client and the server share the
laptop with everything else that runs on it. So version 2's
`FLUSHALL` adds nothing measurable to the latency of other clients.

While another client runs `KEYS`, version 2 answers `PING`s more
slowly: the slowest took 6 milliseconds. This isn't a stall: even when we
lowered the stall detector's threshold to 2 milliseconds, it reported
nothing, so no task ran that long. Yet a `PING` that arrives while `KEYS`
runs is answered only after about 3.5 milliseconds, seven task quotas.
Where does the time go? We added timestamps to a copy of Shoal, and
[Figure](#fig:ping-steps) shows what they recorded. Reading a request
isn't one task, but a chain of them: Seastar reads the socket, then
`read_more()`, `read_line()` and `read()` each resume when the function
they await returns, and only then does `handle_connection()` get the
request. Each step makes the next one ready, and the next one goes to the
back of the queue, behind `KEYS`, so it waits for a whole quota of
`KEYS`, just as the heartbeat did in the experiment. Even the first
step waits: the poll that finds the data comes after `KEYS` yielded, so
`KEYS` is ahead of it in the queue.

![How a task quota of half a millisecond becomes a `PING` that takes 3.5
milliseconds, while `KEYS` runs on the same shard. The reactor polls
between the slices of `KEYS`. Each step of reading the `PING` is a short
task (a dark mark), and each makes the next step ready, which waits in
the queue behind `KEYS` for a whole quota. The reply leaves at the poll
after `handle_connection()` writes it.](figures/ping-steps.svg){#fig:ping-steps}

With `--task-quota-ms 0.1`, each of these waits is five times shorter,
and fewer than six `PING`s in 400,000 took more than a millisecond and a
half. A better way to protect the
latency of short requests from long ones is to put them in separate
[*scheduling groups*]{index="scheduling group"}, and tell Seastar how to share the CPU between them,
as [chapter](#ch:scheduling-groups) shows.

### A stall we didn't write

The stall detector found one more stall in Shoal, and not in a command
we would have suspected. In two of three runs, while `valkey-benchmark`
loaded the keys, it reported a stall of about 70 milliseconds in `SET`.
The backtraces showed where: deep inside
`std::unordered_map`, which rehashes all its keys at once whenever it
grows. The more keys, the longer each rehash takes; with a million keys,
it takes tens of milliseconds. Redis avoids this with a hash table of
its own, which moves the keys to the larger table a few at a time, as
part of the commands that follow. Shoal will live with this stall for
now, because it delays very few requests. The table grows only when its
number of keys doubles, so growing to a million keys rehashes about twenty
times, and only the last few of those rehashes are long. In a server that
runs for hours, these stalls add up to a fraction of a second, and delay
far fewer than one request in a thousand: they show in the slowest
request, but not in the p99, or even the p99.9, latency.
[Chapter](#ch:stalls) returns to it, and to the general problem of
stalls in code you didn't write.

## How the reactor talks to the kernel {index="reactor backend"}

When the reactor polls, it asks the kernel what happened, and when it has
nothing to do, it asks the kernel to put the thread to sleep until
something happens. Linux offers several interfaces for both, and Seastar
can use any of them, through a [*reactor backend*]{.index .main}. The first line that a
Seastar program logs tells which one it uses, and
[`--reactor-backend`]{index="command-line options!`--reactor-backend`"} chooses another:

- **`epoll`**[]{index="reactor backend!`epoll`"}[]{index="`epoll`"}, the oldest, uses the interface that event-driven servers
  have used since 2002. `epoll` tells the reactor which connections are
  ready to be read or written, and the reactor then reads and writes them
  with ordinary non-blocking system calls. But `epoll` doesn't work for
  files on disk: Linux refuses to watch an ordinary file with `epoll`,
  and a "non-blocking" read of a file still waits for the disk. So with
  this backend, disk I/O goes through Linux's asynchronous I/O interface, [*Linux AIO*]{.index .main}.[]{index="AIO|see{Linux AIO}"} A helper thread waits for the
  task quota's timer, to raise the `need_preempt()` flag.
- **`linux-aio`**[]{index="reactor backend!`linux-aio`"} does everything through Linux AIO, which since Linux
  4.18 can also report that a connection is ready. It collects network
  and disk events with the same system call, and needs no helper thread.
  Each shard reserves room for many AIO requests, and the system-wide
  limit on them, [`/proc/sys/fs/aio-max-nr`]{index="`aio-max-nr`"}, may need raising on machines
  with many cores.
- **`io_uring`**[]{index="reactor backend!`io_uring`"}[]{index="io_uring"} uses Linux's newest asynchronous interface, which
  handles both network and disk I/O. It is the default when the kernel
  supports it, and Seastar was built with the `liburing` library, which
  Seastar's `install-dependencies.sh` installs.
- **`asymmetric_io_uring`**[]{index="reactor backend!`asymmetric_io_uring`"} also uses io_uring, but moves the system
  calls, and the kernel's work behind them, to dedicated cores, which
  [`--async-workers-cpuset`]{index="command-line options!`--async-workers-cpuset`"} names; the shards only queue their requests,
  and collect the results. Several shards share each of these cores. It
  refuses to start without that option: this is the backend of exercise 4
  of [chapter](#ch:getting-started).

Whatever the backend, reading from a connection doesn't always wait for
the kernel to say that data has arrived. When a read fills the whole
buffer it was given, there is probably more data waiting, so the
reactor's next read on that connection tries the system call at once.
Seastar calls this [*speculation*]{.index .main}. The asymmetric backend never
speculates: all of its I/O goes to the dedicated cores.

Shoal version 2, on one core and measured as in
[chapter](#ch:coroutines), gave these results with each backend, the
best of three runs:

| Backend | `SET` per second | `GET` per second |
|---|---|---|
| `epoll` | 3,480,000 | 4,130,000 |
| `linux-aio` | 3,520,000 | 4,180,000 |
| `io_uring` | 3,660,000 | 4,360,000 |
| `asymmetric_io_uring`, with a second core | 3,970,000 | 4,850,000 |

The three ordinary backends are within 6% of each
other, so for a server like Shoal, the choice of backend matters less
than one might expect. The asymmetric backend did 8% to 11% more on one
shard, but it used a second core to do it, and that core could
instead have run a second shard. The asymmetric backend is meant for
shards that are busy with computation rather than I/O, so that moving
the kernel's work elsewhere leaves them more time for their own. [Chapter](#ch:deploying-and-tuning)
discusses how to choose.

Disk I/O deserves a word too. Seastar reads and writes files with
[*direct I/O*]{.index}, which bypasses the kernel's page cache. With Linux AIO,
this is essential: only direct I/O is truly asynchronous there, and an
ordinary read of a file may block the shard inside the system call that
was supposed to start it in the background. Direct I/O also leaves the
caching to the application, which knows best what is worth keeping in
memory. [Chapter](#ch:files-and-storage) explains how to work with it.

## Exercises

1. Build Shoal with the second attempt at `KEYS`, the one that keeps an
   iterator across `coroutine::maybe_yield()`, and run its tests a few times with
   `./run 2 -k keys_while_changing`. How does the test fail? Can you tell,
   from the failures, which of the two problems described above happened?

2. Run `preemption --work maybe_yield` with `--task-quota-ms 0.1`, and
   with `--task-quota-ms 5`. How late is the most delayed heartbeat, and how
   many beats are there? Explain the numbers. Then measure the other side
   of the compromise: benchmark Shoal as in [chapter](#ch:coroutines),
   with the default quota and with `--task-quota-ms 0.05`. How much
   throughput does the shorter quota cost, and why?

3. Start Shoal on one core (`--cpuset 0 --port 6390`), and watch its CPU use in `top`
   while it is idle, and while
   `valkey-cli -p 6390 -r 5000 -i 0.001 PING > /dev/null` sends it a
   `PING` every millisecond. Then repeat both with
   `--idle-poll-time-us 0`. How much CPU time does each `PING` cost, with
   and without idle polling? What is the rest of that time spent on?

4. Load Shoal with 4 million keys (`-n 4000000 -r 40000000`), running it
   with `-m4G --blocked-reactor-notify-ms 2
   --blocked-reactor-reports-per-minute 1000`, so that the detector
   doesn't stop after five reports. How long are the stalls when the hash
   table grows, and how do they grow with the number of keys? (Remember
   that one long stall is reported several times. And not every stall is
   the hash table's. To see where a stall was,
   give the addresses of its backtrace to `addr2line -Cfi -e build/shoal`,
   which prints the functions and source lines they belong to.) How would
   you build a hash table that grows without stalling?

5. Redis's `FLUSHALL ASYNC` replies at once, and frees the memory in the
   background. Make Shoal's `FLUSHALL ASYNC` do the same: reply, and then
   destroy the old data in a coroutine that runs in the background. What
   must you be careful about? (Hint: who owns `old` now?)
   [Chapter](#ch:concurrency-within-a-shard) discusses background work.
