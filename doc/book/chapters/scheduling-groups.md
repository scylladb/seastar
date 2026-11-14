# Scheduling groups {#ch:scheduling-groups}

A server does two kinds of work. Some of it, such as answering a
client's request, someone is waiting for, and every millisecond it takes
shows in the server's latency. The rest, such as compacting files,
rewriting a log or building an index, has to be done too, but nobody
minds if it takes a little longer. On a shard that has both, the second
kind can easily delay the first: as
[chapter](#ch:how-seastar-runs-your-code) measured, while Shoal's `KEYS`
ran, a `PING` from another client took 3.5 milliseconds instead of a few
microseconds, although `KEYS` yielded at every task quota, as it should.

That chapter's reactor had a single queue of tasks, which it ran in the
order in which they became ready. A Seastar application can divide its
work into several *scheduling groups*, each with a queue of its own, and
tell the reactor what part of the CPU each group gets, in *shares*. The
same groups divide the disk:
[chapter](#ch:io-scheduler)'s I/O scheduler serves each group's
requests in proportion to the same shares. This chapter shows how the
reactor divides the CPU among groups, how it decides which group a piece
of code belongs to (which is where most of the surprises are), and the
rest of what Seastar offers around groups: shares that change as the
program runs, groups of groups, and data kept for each group. Shoal's
version 11 puts `KEYS` in a group of its own, and gets a long background
job that clients mustn't notice: rewriting its log, as Redis's
`BGREWRITEAOF` does.

## One queue {index="scheduling group!why"}

A single queue divides the CPU among tasks, not among the kinds of work
that they do. The program `fibers.cc` runs, for a second, one loop that
does small pieces of work, against ten such loops, which `loops()` runs
together, first all in the same group, and then the one loop in a group
of its own, and the ten in another, with the same number of shares (the
next section shows how a program makes groups):

```{.cpp include="examples/scheduling-groups/fibers.cc" snippet="loops"}
```

```{.cpp include="examples/scheduling-groups/fibers.cc" snippet="compete"}
```

```
one group: one loop 9%, ten loops 91%
two groups: one loop 50%, ten loops 50%
```

In one queue, each loop gets its turn in it, so ten loops get ten times
the CPU of one, and a kind of work gets more of the CPU the more fibers
it runs at once. In groups, each group gets its share, whatever number
of fibers it runs. (Seastar's old tutorial, `doc/tutorial.md`, made the
same point with the same experiment.)

## Groups and shares {index="scheduling group" index="shares"}

This program makes three groups, with 1,000, 500 and 250 shares, and
runs the same loop in each of them, on one shard, for one second. The
loop does a small piece of computation, again and again, and counts the
pieces; `maybe_yield()`, which [chapter](#ch:how-seastar-runs-your-code)
introduced, lets the reactor run other tasks whenever the loop has run
for a task quota, half a millisecond:

```{.cpp include="examples/scheduling-groups/cpu-shares.cc" snippet="spin"}
```

[`with_scheduling_group()`]{index="`seastar::with_scheduling_group()`"},
which [chapter](#ch:io-scheduler) used, runs a function in a group, so
`compete()` runs one `spin()` in each. It's one of the functions that
[chapter](#ch:object-lifetime) warned about: it calls the function in a
task of the group, and frees the task, with the function, as soon as the
call returns, which for a coroutine is when it first waits. `spin()` is
a plain function, which takes its argument by value, so it's safe; a
lambda coroutine that captures anything needs `this auto`:

```{.cpp include="examples/scheduling-groups/cpu-shares.cc" snippet="compete"}
```

and the program runs it twice, first with all three groups, and then
without the first:

```{.cpp include="examples/scheduling-groups/cpu-shares.cc" snippet="groups"}
```

[`create_scheduling_group()`]{index="`seastar::create_scheduling_group()`"}
makes a group on every shard, and returns a
[`scheduling_group`]{index="`seastar::scheduling_group`" .main}, from
`<seastar/core/scheduling.hh>`, a small handle that any shard can use.
The program prints:

```
all three busy:  high 57%  medium 29%  low 14%
high idle:  medium 67%  low 33%
```

Each group got exactly its part of the shares: 1,000 of 1,750 is 57%,
500 is 29% and 250 is 14%. When `high` had nothing to do, the other two
divided its part between them, two to one, as their shares are. So
shares aren't a reservation, and a group doesn't get its part whether
it needs it or not: they say only how the CPU is divided among the
groups that have work to do. A group alone on a shard gets the whole
CPU, whatever its shares. In particular, a background group with few
shares still runs at full speed when nothing else does, which is what we
want from it.

Every program has groups even if it makes none:
[`default_scheduling_group()`]{index="`seastar::default_scheduling_group()`" .main},
from `<seastar/core/scheduling.hh>`, named `main`, with 1,000 shares,
which runs everything that isn't put in another group, and an `atexit`
group that Seastar uses as it exits. A program can have 16 groups in
all, including these two, unless Seastar was built with more
(`configure.py --scheduling-groups-count`, which
[appendix](#app:building-seastar) lists). A group that's no longer
needed can be destroyed with
[`destroy_scheduling_group()`]{index="`seastar::destroy_scheduling_group()`"},
as the program does at its end, and then its slot can be used again.

### How the reactor chooses {index="scheduling group!virtual runtime"}

The reactor keeps, for each group, the CPU time that it has used, divided
by its shares: its *virtual runtime*. A group with 1,000 shares that ran
for a millisecond adds one unit; a group with 250 shares adds four. When
the reactor is ready to run tasks, it picks the group whose virtual
runtime is the smallest, runs that group's tasks until its queue is
empty or a task quota is up, adds what they used, and chooses again. A
group with few shares sees its virtual runtime grow fast, and waits
longer for its next turn, so over time every busy group's virtual
runtime grows at the same rate, and each group's part of the CPU is
proportional to its shares. Linux's scheduler, CFS, divides the CPU
among processes in the same way.

One more rule keeps an idle group from saving up credit. A group that
had nothing to do, and so used no time, would have a small virtual
runtime when it wakes up, and could then keep the CPU to itself for a
long time. So when a group's queue gets a task after being empty, the
reactor raises its virtual runtime to that of the group that ran last.
It gets ahead of the others only by being idle while they run, and
only for its next turn: just what a group of short requests needs to be
answered quickly.

## Which group runs your code {index="scheduling group!inheritance"}

The program above told Seastar which group should run `spin()`. In a
real program, most code doesn't say: it's a continuation, a coroutine
resumed after a `co_await`, a message from another shard, or a timer's
function. The rule behind all of them is simple: a task's group is
chosen when the task is created, and it's the group that was running
when it was created,
[`current_scheduling_group()`]{index="`seastar::current_scheduling_group()`" .main},
from `<seastar/core/scheduling.hh>`. This program shows what that means
in each case. It makes a group, `background`, and runs a coroutine in it:

```{.cpp include="examples/scheduling-groups/which-group.cc" snippet="inherit"}
```

`show()` prints the name of the current group, and the program prints:

```
main(), run by app_template:                 main
a coroutine, called in background:           background
the same coroutine, after a co_await:        background
a continuation it created:                   background
its message, run on shard 1:                 background
```

A coroutine is a task, created when the coroutine starts, and each time
it resumes after a `co_await`, the same task runs again, in the same
group. A continuation made by `then()` is a task too, created in the
group that called `then()`. And `smp::submit_to()` sends the other
shard a task created on this one, in this one's group; since every
group exists on every shard, the message runs on shard 1 in
`background`. So a fiber, the chain of tasks that serve one request or
one job, stays in the group it started in, wherever it goes.

The exceptions are code that was created earlier, elsewhere, and is only
started now. A timer's function is created with the timer, and runs in
the group that was current then, not in the group that arms it:

```{.cpp include="examples/scheduling-groups/which-group.cc" snippet="timer"}
```

```
a timer made in main:                        main
a timer made for background:                 background
```

So a timer that belongs to background work, but is a member of an
object constructed when the program started, runs its function in
`main`, unless it's given its group, as `made_for_bg` was. In the same
way, a fiber that waits, on a semaphore, a condition variable or a
future, resumes in its own group, whichever group wakes it up:

```{.cpp include="examples/scheduling-groups/which-group.cc" snippet="wake"}
```

```
a waiter in main, woken by background:       main
```

which is usually what we want: waking up a client's request shouldn't
turn it into background work. But it means that work done on behalf of
another fiber runs in the group of whoever does it. If a background job
hands its requests to a fiber that a client started, such as a queue's
consumer, the work runs in the client's group, and the background job's
low shares protect nobody. Shoal's version 11 runs into exactly this,
below.

A function can also change groups as it runs. A `seastar::thread` gets
its group from its attributes,
[`thread_attributes::sched_group`]{index="`seastar::thread_attributes`!`sched_group`"},
and can move itself to another group with
[`thread::switch_to()`]{index="`seastar::thread::switch_to()`" .main},
from `<seastar/core/thread.hh>`; a coroutine does the same with
[`coroutine::switch_to()`]{index="`seastar::coroutine::switch_to`" .main},
from `<seastar/coroutine/switch_to.hh>`, which returns the group it
switched from:

```{.cpp include="examples/scheduling-groups/which-group.cc" snippet="switch"}
```

```
a thread started for background:             background
the thread, after switch_to(main):           main
main(), after coroutine::switch_to(bg):      background
```

`coroutine::switch_to()` changes the group of the coroutine's own task,
so the rest of the coroutine runs in the new group, and so does
everything it calls and creates. Its caller isn't affected: when the
coroutine returns, the caller resumes in its own group.

Two more cases complete the picture. A disk request's class, which
[chapter](#ch:io-scheduler)'s I/O scheduler uses to divide the disk, is
the group that's current when the read or the write is submitted: the
I/O follows the CPU work that asked for it. And code that the reactor
runs outside any task, such as the poller that finds that a socket has
data, runs in the default group; but it only makes ready the futures
that fibers wait on, and the fibers resume in their own groups. Until
Seastar's API level 7, in 2023, I/O had classes of its own, separate from
the CPU's groups, which a program created and passed to every read and
write; [chapter](#ch:upgrading-seastar) lists that change.

In short:

| Code | Runs in |
|---|---|
| A coroutine, also after each `co_await` | The group that was current when it was called |
| A continuation made by `then()` | The group that called `then()` |
| A message sent with `submit_to()` | The sender's group, on the other shard |
| A timer's function | The group that was current when the timer was made, unless it's given one |
| A fiber woken from a wait | Its own group, whoever wakes it |
| A `seastar::thread` | The group in its attributes, until it calls `switch_to()` |
| A disk request | The group that's current when it's submitted |
| The reactor's pollers | The default group; the fibers they wake run in their own |

## Shoal, version 11 {index="Shoal!version 11"}

Version 11 puts two kinds of Shoal's own work in groups of their own,
with 100 shares each, a tenth of the default group's 1,000, where the
clients' requests run:

```{.cpp include="shoal/v11/commands.hh" snippet="groups"}
```

```{.cpp include="shoal/v11/main.cc" snippet="create-groups"}
```

### `KEYS` in its own group

`KEYS` looks at every key of every shard, and builds a reply that may
be huge; [chapter](#ch:how-seastar-runs-your-code) made it yield, so that
it doesn't stall other clients, but they still waited a whole task quota
of `KEYS` for each step of reading a request, 3.5 milliseconds for a
`PING`. Now `KEYS` switches its coroutine to the keys group as it starts:

```{.cpp include="shoal/v11/commands.cc" snippet="keys-group"}
```

and the rest of it, including the messages it sends the other shards to
collect their keys, runs in that group.

We measured it as [chapter](#ch:how-seastar-runs-your-code) did: Shoal on
one shard, with about 950,000 keys, one client sending `KEYS 'nomatch*'`
every 100 milliseconds, and another sending `PING`s, one at a time, whose
latency we recorded, separately for the `PING`s sent while a `KEYS` was
running (the best of three runs; the three hardly differed):

| | `PING` during `KEYS`: median | Slowest 1% | `KEYS` |
|---|--:|--:|--:|
| Version 10 | 3.49 ms | 3.60 ms | 63.4 ms |
| Version 11 | 1.00 ms | 1.03 ms | 64.3 ms |

A `PING` no longer waits a task quota of `KEYS` for each of its steps.
When the reactor's poll finds it, the default group, which was idle, has
a smaller virtual runtime than the keys group, which has just run, so the
default group runs first, and it runs all of the `PING`'s steps in one
turn, since each step makes the next one ready in the same queue. What
remains are two task quotas: the slice of `KEYS` that was running when
the `PING` arrived, and one more before the poll that sends the reply.
`KEYS` itself took 1.5% longer, and other `PING`s still took 8
microseconds.

### Rewriting the log {index="log rewrite" index="`BGREWRITEAOF`"}

[Chapter](#ch:files-and-storage)'s log only grows. A key set a million
times leaves a million `SET`s in it, and Shoal replays all of them when
it starts. Redis's answer is `BGREWRITEAOF`: rewrite the append-only file
as the shortest list of commands that recreates the data, one per key,
and replace the old file with it. Redis does it in a child process,
which writes a copy of the data that the fork froze, and that is also the
source of a famous problem: the child's writes keep the disk busy, and
with `appendfsync always`, every command waits for an `fsync()`, which
now waits behind them. Redis even has an option,
`no-appendfsync-on-rewrite`, which stops waiting for the disk during a
rewrite, and so risks losing the changes of that time in a crash.

Shoal has no child process, and needs none. Its rewrite is a fiber on
each shard, in the rewrite group, which writes a new log, while the
shard goes on serving its clients:

```{.cpp include="shoal/v11/commands.hh" snippet="store-rewrite"}
```

The data isn't frozen while the rewrite reads it, so two things keep the
new log right. First, while the rewrite runs, every change goes to both
logs, the old one, whose commits clients wait for, and the new one, in
which it's recorded after whatever the rewrite has written so far:

```{.cpp include="shoal/v11/commands.cc" snippet="record"}
```

This has a price in memory: during a rewrite, each change waits in
memory twice, in each log's tail, until each log has committed it. A
server that receives large values while it rewrites needs room for both
copies; Shoal's tests write ten values of 60 MB at once, which, with
Shoal's 512 MB a shard, fit beside one copy but not two.

And second, the rewrite writes each key as requests that *replace* it:

```{.cpp include="shoal/v11/commands.cc" snippet="write-key"}
```

A key that changed before the rewrite reached it has the change in the
new log, followed by the key's current value, which replaces it. A key
that changes after the rewrite wrote it has its current value followed
by the change. Either way, replaying the new log gives the key's current
value. The rewrite looks at one bucket of the hash table at a time, as
`KEYS` does, since other requests change the table while it yields:

```{.cpp include="shoal/v11/commands.cc" snippet="rewrite"}
```

Every megabyte, the rewrite writes what it has to the file, with a new
method of the log, `write()`, which, unlike `commit()`, doesn't flush:
the new log needs to be durable only when it replaces the old one, and
the hand-over, below, commits it then. Our first version flushed every
megabyte, as Redis's rewrite does every 4 MB (its
`aof-rewrite-incremental-fsync`, which spreads the cost of flushing a file
written through the page cache); Shoal's writes go straight to the disk,
and the flushes only made the clients' own flushes wait. Without them,
the slowest 1% of the clients' `SET`s, measured as below, took 8.6
milliseconds instead of more than 12.

`BGREWRITEAOF` starts the rewrite on every shard, in the rewrite group,
and replies at once, as Redis does; `INFO` tells when it's done:

```{.cpp include="shoal/v11/commands.cc" snippet="bgrewriteaof"}
```

### The hand-over {index="log rewrite!hand-over"}

The hardest part is the end. The new log must replace the old one
without losing a change that a client was told is on the disk. A client
with `appendfsync always` is told so when a commit of the old log ends.
If the new file took the old one's name after that commit, but before
the new log had that change on the disk, a crash at that moment would
lose it. So during the hand-over, no commit of the old log may end:

```{.cpp include="shoal/v11/log.cc" snippet="hand-over"}
```

Holding the old log's commit semaphore, the hand-over commits the new
log, which has every change that the old one received since the rewrite
began, renames it over the old file, and makes the rename durable. Then
it sets `_successor`, so that the commits waiting for the semaphore,
when they get it, commit the new log instead. A commit now also keeps
track of how much of the file is durable, so that it flushes what
`write()` wrote without flushing:

```{.cpp include="shoal/v11/log.cc" snippet="commit"}
```

The clients' commits wait for the hand-over, which is why the rewrite
switches to the default group before it begins: in the rewrite group,
with its few shares, the hand-over would make clients wait for work that
the scheduler considers unimportant, the trap that the previous section
described. Work that someone waits for belongs in their group.

### What it bought

We measured the clients' latency while Shoal rewrote its log again and
again, a new rewrite as soon as the last one ended, with about 3 million
keys of 100 bytes, on one shard, with the disk's properties from
[chapter](#ch:io-scheduler), and 32 clients. First, `SET`s with
`--appendfsync always`, each of which waits for a flush (the best of
three runs; the others were close):

| Rewrite | `SET`s a second | Median | Slowest 5% | Slowest 1% |
|---|--:|--:|--:|--:|
| None | 5,303 | 5.95 ms | 6.08 ms | 6.98 ms |
| In the default group | 4,782 | 6.04 ms | 8.02 ms | 8.82 ms |
| In the rewrite group | 5,022 | 6.02 ms | 7.92 ms | 8.59 ms |

The group helped, but not much, because what these clients wait for is
the flush, which Seastar doesn't schedule: a flush is a system call that
the kernel serves, and it takes longer when the disk is busy with the
rewrite's writes, whichever group sent them. (Valkey, whose rewrite is a
child process on another core, slowed from 6.1 to 8.0 milliseconds at
the slowest 1%: about the same cost.) Then `GET`s, which wait for
nothing but the CPU:

| Rewrite | `GET`s a second | Median | Slowest 5% | Slowest 1% |
|---|--:|--:|--:|--:|
| None | 276,893 | 0.071 ms | 0.079 ms | 0.111 ms |
| In the default group | 110,522 | 0.111 ms | 1.67 ms | 3.02 ms |
| In the rewrite group | 121,146 | 0.111 ms | 0.99 ms | 1.02 ms |
| In the rewrite group, task quota 0.1 ms | 196,483 | 0.135 ms | 0.20 ms | 0.22 ms |

Here the group cut the slowest 1% from 3 milliseconds to 1. In the
default group, the rewrite's tasks were in the same queue as the
clients', and each step of a request waited behind a task quota of the
rewrite, as `PING`'s steps waited behind `KEYS`. But the clients still
lost more than half of their throughput, and the rewrite group's shares
made no difference: with 10 shares, or 1,000, the results were the same
as with 100. Shares divide the CPU among groups that have work at the
same time, and these clients, each of which waits for its reply before
sending the next request, leave the default group idle for moments
between requests. In such a moment the rewrite's group runs, alone, and
keeps the CPU for a whole task quota, since the reactor polls for new
requests only when the quota ends, and requests that arrive meanwhile
wait for it: the same two quotas that `KEYS` cost a `PING`. A shorter
quota, `--task-quota-ms 0.1`, which [chapter](#ch:how-seastar-runs-your-code)
tried, shortened those waits, and the last row shows it. Even clients
that send 16 requests at a time, and so keep the default group busier,
gained only a few percent from fewer shares. So groups keep background
work from getting *ahead* of requests in the queue, which is most of
the latency they cost; what's left is the task quota, which is the
price of polling less often.

### Stopping

[Chapter](#ch:clean-shutdown) promised that Shoal's shutdown would stop a
rewrite, rather than wait for it to finish, as Redis's `SHUTDOWN` kills
its rewriting child. The rewrite holds the store's background gate,
which `store::stop()` closes after requesting `stopping`, and the rewrite
checks `stopping` after every bucket. When it stops, it drops the new
log and removes its file, and the old log, complete, stays. If Shoal
crashes during a rewrite, the next start finds the unfinished file, and
removes it.

With about 3 million keys, on one shard, a rewrite took 1.9 seconds
when nothing else ran. A `SHUTDOWN` sent 0.2 seconds into a rewrite
stopped Shoal 0.3 seconds later, rather than when the rewrite would have
ended, and when Shoal started again, every key was there.

## Shares that change {index="shares!dynamic"}

A group's shares can be changed at any time, with
[`scheduling_group::set_shares()`]{index="`seastar::scheduling_group`!`set_shares()`" .main},
from `<seastar/core/scheduling.hh>`. Unlike creating a group, it changes
the shares only on the shard that calls it, so each shard can adjust
its own. Fixed shares make a background job slow exactly when the server
is busy, which is when its work piles up fastest. ScyllaDB's answer is a
*backlog controller*: every so often, a timer measures how much work the
job has piled up, its backlog (for compaction, roughly how much data
awaits compaction), maps it to a number of shares through a few points of
a line, and calls `set_shares()`. A small backlog gets few shares, and
clients get almost all of the shard; a growing one gets more, so the
backlog stops growing before it hurts, for example before the disk fills
with data waiting to be compacted. The idea fits in a few lines, and
exercise 3 gives Shoal's rewrite shares that can change.

A group's disk bandwidth can be capped too:
[`scheduling_group::update_io_bandwidth()`]{index="`seastar::scheduling_group`!`update_io_bandwidth()`" .main}
limits the bytes a second that the group's requests may transfer, on
all the shards together, whether or not the disk is busy. Shares say
how to divide the disk when it's busy; a cap keeps a background job from
using more than its part even when it's idle, for example to leave the
disk's bandwidth to another program on the same machine. Redpanda caps
some of its background work this way.

## Groups of groups {index="scheduling supergroup"}

When a program has many groups, their shares become hard to reason
about: adding a group takes a part of the CPU from all the others. A
*supergroup*, made by
[`create_scheduling_supergroup()`]{index="`seastar::create_scheduling_supergroup()`" .main},
from `<seastar/core/scheduling.hh>`, with shares of its own, holds groups
created with it as their parent: the supergroup competes with the other
groups and supergroups for its part of the shard, and its groups divide
that part among themselves, by their shares. ScyllaDB, for example,
puts the CQL statements of its users in one supergroup, and the work it
does for maintenance, such as streaming data to a new node and backups,
in another with a fifth of the shares; adding a maintenance group, or a
user's group, then changes only how its own supergroup's part is
divided. Supergroups are recent, added to Seastar in 2025; exercise 1
measures one.

## Data for each group {index="scheduling group!data for each"}

Sometimes a value should be kept separately for each group, such as
statistics, or a limit, without passing the group around: a request's
code knows only that it runs in some group. A
[`scheduling_group_key`]{index="`seastar::scheduling_group_key`" .main},
from `<seastar/core/scheduling.hh>`, gives every group, on every shard, a
value of its own, and finds the current group's. This program counts the
requests handled by each group:

```{.cpp include="examples/scheduling-groups/group-data.cc" snippet="stats"}
```

The key is made once, when the program starts, by
[`scheduling_group_key_create()`]{index="`seastar::scheduling_group_key_create()`"}, from a configuration that
[`make_scheduling_group_key_config()`]{index="`seastar::make_scheduling_group_key_config()`"}
builds for a type. Every group gets a value, made by the type's
constructor, including groups created later:

```{.cpp include="examples/scheduling-groups/group-data.cc" snippet="create"}
```

[`scheduling_group_get_specific()`]{index="`seastar::scheduling_group_get_specific()`"},
from `<seastar/core/scheduling_specific.hh>`, finds the current group's
value, and a group's `get_specific()`, any group's:

```{.cpp include="examples/scheduling-groups/group-data.cc" snippet="handle"}
```

The program handles three requests in one group, and two in another,
and prints:

```
interactive: 3 requests
batch: 2 requests
```

ScyllaDB keeps per-group values this way for the statistics of each of
its users' service levels, which are groups.

## Execution stages {index="execution stage"}

A server's request goes through a long chain of functions, and when a
shard runs one request after another, each brings its own code into the
CPU's instruction cache, evicting the previous request's. An *execution
stage*, made by
[`make_execution_stage()`]{index="`seastar::make_execution_stage()`" .main},
from `<seastar/core/execution_stage.hh>`, wraps a function so that
calling it only queues the call, and returns a future; the queued calls
run together, in a batch, when enough of them have queued or when the
reactor polls, so the function's code is loaded into the cache once for
the batch. A plain execution stage runs all its calls in one group, the
one current when it was made, or one given to it; the kind that
ScyllaDB uses,
[`inheriting_concrete_execution_stage`]{index="`seastar::inheriting_concrete_execution_stage`"},
keeps a queue for each group, and runs each call in its caller's group,
so batching doesn't move work between groups.

Execution stages came with great promise. Paweł Dziepak added them to
Seastar in 2017, and made five of ScyllaDB's steps execution stages: the
CQL statements, the CQL server's requests, the storage proxy's writes,
and the database's writes and reads. The merge reported, on ScyllaDB's
`perf_simple_query` benchmark on four shards, 242,876 writes a second
instead of 83,018, and 168,258 reads instead of 61,709, nearly three
times the throughput, and half the median latency of writes at a light
load. It also warned that "the real life improvements aren't as
good because it is much harder to collect sufficiently high number of
operations in a batch": a benchmark that sends the same request again
and again batches perfectly, and a real workload, mixing requests of
different kinds, doesn't.

They didn't spread. ScyllaDB has added no execution stage since 2018,
when it made its stages keep their callers' groups, and in 2021 it
removed the database's read stages, when it began admitting reads only
as fast as the CPU can serve them: cache reads now run essentially one
at a time, which also keeps the CPU on one read's code. Redpanda, and
Ceph's Crimson, use none; smf, an RPC framework by Redpanda's founder,
used them in its client and server, and Redpanda's own code doesn't. The
issues that Seastar's developers still see with them are about their
upkeep: each stage reports metrics for every group, "hundreds of metrics
per shard", and a group renamed, or destroyed and made again with the
same name, has broken them. An execution stage is worth trying where a
hot path is long, and calls to it pile up, as they do in a server under
heavy load; measure it on a realistic mix of requests, not one request
repeated. And measure first whether there's anything to gain: `perf
stat` counts the cycles that the CPU spent waiting for the instruction
cache (`icache_data.stalls`, on Intel's processors). In Shoal, serving
pipelined `GET`s, or a mix of `GET`, `SET`, `LPUSH` and `LRANGE`, they
were 0.5% to 0.6% of all its cycles: its code fits in the cache, and an
execution stage couldn't save more than that.

## Watching the scheduler {index="scheduling group!metrics"}

Seastar exports metrics for each group, which
[chapter](#ch:observability) shows how to collect, labeled with the
group's name: `scheduler_runtime_ms`, the CPU time it used,
`scheduler_waittime_ms`, the time it had nothing to run, and
`scheduler_starvetime_ms`, the time it had work but waited for other
groups; `scheduler_shares`, `scheduler_queue_length` and
`scheduler_tasks_processed`; and the time its tasks spent beyond the
task quota. A group's short name, four characters given to
`create_scheduling_group()`, appears in log lines, next to the shard:
Shoal's log says `[shard 0:main]`, and a line written by the rewrite
would say `[shard 0:rewr]`. A group can be renamed, with
[`rename_scheduling_group()`]{index="`seastar::rename_scheduling_group()`"}, as ScyllaDB does when an administrator
renames a service level.

## Exercises

1. Give `cpu-shares.cc` a supergroup with 1,000 shares, and create
   `medium` and `low` in it, leaving `high` alone. Before you run it,
   work out what each group should get when all three are busy, and when
   `high` is idle. Were you right?

2. In `which-group.cc`, a fiber started in the default group waits on a
   semaphore that a fiber in `background` signals, and resumes in the
   default group. Write the opposite: a fiber in `background` that waits
   on a `shared_future` set by a timer made in the default group. In
   which group does it resume, and in which group does the timer's
   function run? Check with the program.

3. Give Shoal a command, `REWRITESHARES n`, that sets the rewrite
   group's shares on every shard, with `set_shares()`, even while a
   rewrite runs. With clients keeping Shoal busy, as in this chapter's
   measurement, how long does a rewrite take with 10, 100 and 1,000
   shares, and how slow are the clients' `SET`s? Which would you choose,
   and what could Shoal measure to choose by itself?

4. Instead of low shares, cap the rewrite's disk bandwidth with
   `update_io_bandwidth()`, at 100 MB a second, keeping its 100 shares.
   How do the `SET`s' latency, and the rewrite's duration, compare with
   this chapter's? When would you choose a cap, and when shares?

5. Redis starts a rewrite by itself when the log has grown to twice its
   size after the last rewrite (its `auto-aof-rewrite-percentage`, 100 by
   default). Give Shoal an option that does the same, on each shard, and
   a test.
