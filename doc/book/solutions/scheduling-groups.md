# Scheduling groups {#sol:scheduling-groups}

## Exercise 1

`high` and the supergroup have 1,000 shares each, so when all three
groups are busy, they divide the shard in half: 50% for `high`, and 50%
for the supergroup, which its groups divide by their shares, 500 to
250, so 33% for `medium` and 17% for `low`. When `high` is idle, the
supergroup has the shard to itself, and its groups divide all of it the
same way: 67% and 33%, as without the supergroup. The program:

```{.cpp include="examples/solutions/scheduling-groups-ex1.cc"}
```

prints exactly that:

```
all three busy:  high 50%  medium 33%  low 17%
high idle:  medium 67%  low 33%
```

Without the supergroup, `high` got 57%: its 1,000 shares were weighed
against the 750 of the other two together. In the supergroup, `medium`
and `low` count as one, with the supergroup's 1,000 shares, however many
groups it has, and whatever their shares. That's what makes supergroups
useful: adding a group to one changes only how that supergroup's part is
divided, and not the others' parts.

## Exercise 2

```{.cpp include="examples/solutions/scheduling-groups-ex2.cc"}
```

prints:

```
the waiter, before it waits:         background
the timer's function:                main
the waiter, after it resumed:        background
```

The timer was made in the default group, so its function runs there,
although the waiter, in `background`, armed it. The function's
`set_value()` makes the waiter's future ready, which schedules the
waiter's coroutine to resume: its own task, created when the coroutine
started, in `background`. So the waiter resumes in its own group, as
the waiter of the chapter's semaphore did, the other way around. Waking
a fiber never moves it to the waker's group; it's the code that runs on
behalf of others, here the timer's function, whose group is decided by
where it was made.

## Exercise 3

The solution is a patch to Shoal version 11,
`examples/solutions/shoal-rewrite-shares.patch`, which adds the command.
`set_shares()` changes a group's shares only on the shard that calls it,
so the command calls it on every shard:

```{.cpp include="examples/solutions/shoal-rewrite-shares.patch" file="commands.cc" snippet="rewriteshares"}
```

We measured it as the chapter did: about 3 million keys, one shard, a
new rewrite as soon as the last one ended, and 32 clients sending `GET`s
(the best of three runs):

| Rewrite group's shares | `GET`s a second | Slowest 1% | Rewrites, each |
|---|--:|--:|--:|
| 10 | 121,344 | 1.02 ms | 2.3 s |
| 100 | 121,146 | 1.02 ms | 2.3 s |
| 1,000 | 120,722 | 1.02 ms | 2.3 s |

The shares made no difference, to the clients or to the rewrite. As the
chapter explains, these clients, each waiting for its reply before it
sends the next request, leave the default group idle for moments, and
the rewrite runs then, whatever its shares, for a whole task quota. With
clients that send 16 requests at a time, the shares mattered a little:
10 shares gave 1,265,022 `GET`s a second, and 1.02 milliseconds at the
slowest 1%, and 1,000 shares 1,219,141 and 1.20 milliseconds.

So with this load, 100 is as good as any. Shares matter when both groups
have work at the same time, all the time, and that is what Shoal could
measure to choose by itself: Seastar's metric `scheduler_starvetime_ms`,
for each group, is the time it had work but waited for other groups. A
default group that starves, while the rewrite runs, says that the
rewrite's shares take time from clients; one that doesn't says that the
rewrite runs only when they're idle, and its shares could be higher,
for a shorter rewrite, at no cost to them. That's a backlog controller of
a different kind, driven by the clients' wait rather than by the work
that piles up.

## Exercise 4

The solution is a patch to Shoal version 11,
`examples/solutions/shoal-rewrite-cap.patch`. A cap is set for the
group, on all the shards together, so `main()` sets it once, after
creating the group:

```{.cpp include="examples/solutions/shoal-rewrite-cap.patch" file="main.cc" snippet="cap"}
```

With `--appendfsync always`, 32 clients and a new rewrite as soon as the
last one ended (the best of three runs):

| Rewrite | `SET`s a second | Slowest 5% | Slowest 1% |
|---|--:|--:|--:|
| None | 5,303 | 6.08 ms | 6.98 ms |
| In the rewrite group | 5,022 | 7.92 ms | 8.59 ms |
| In the rewrite group, capped at 100 MB/s | 5,025 | 7.83 ms | 8.05 ms |

The cap helped the slowest `SET`s a little more than the group alone:
with fewer of the rewrite's bytes in flight at any moment, the clients'
flushes, which wait for the disk, waited less. The rewrite took about as
long: it writes about 430 MB, which under this load took about 6.5
seconds, 66 MB a second on average, below the cap. On an idle server,
where the rewrite took 1.9 seconds, about 230 MB a second, the cap would
make it last at least 4.3.

Shares divide the disk when it's busy, and give the rewrite all of it
when it isn't, which makes the rewrite as short as it can be; a cap
limits the rewrite even on an idle disk, and makes it longer than it
needs to be. A cap is the better choice when what suffers from the
rewrite's bandwidth isn't in its scheduler: the kernel's flushes, as
here, another program on the same disk, or a disk whose properties
weren't measured, and on which shares do nothing.

## Exercise 5

The solution is a patch to Shoal version 11,
`examples/solutions/shoal-auto-rewrite.patch`, which adds the options
`--auto-aof-rewrite-percentage` and `--auto-aof-rewrite-min-size`, as
Redis's. Each shard remembers its log's size after its last rewrite, or
when it opened the log, its base size, and every change it records
checks whether the log has grown by the percentage:

```{.cpp include="examples/solutions/shoal-auto-rewrite.patch" file="commands.cc" snippet="maybe-rewrite"}
```

`BGREWRITEAOF` and the check both start the rewrite in the same way, on
one shard, in the rewrite group:

```{.cpp include="examples/solutions/shoal-auto-rewrite.patch" file="commands.cc" snippet="start-rewrite"}
```

The patch also gives `INFO` Redis's fields `aof_current_size` and
`aof_base_size`, which let the test see the logs' sizes through the
protocol, the same way against Valkey and Shoal:

```{.python include="examples/solutions/shoal-auto-rewrite.patch" file="test_solutions.py"}
```

Unlike Redis, which rewrites at 100% by default, this Shoal doesn't
rewrite by itself unless asked to. With 100% by default, Shoal's own
tests failed: a test that writes ten values of 60 MB at once started a
rewrite in the middle, and during a rewrite every change waits in memory
twice, once for each log, which was more than Shoal's 512 MB a shard
could hold. Redis's default suits a server with memory to spare; it's a
decision for whoever runs it, not one to make for them.

Writing the test showed a difference between Valkey and this Shoal.
Both rewrote as soon as the log reached the minimum size, and both
count in the base size the changes made during the rewrite, which go to
the new log too. But Valkey's rewrite, in a child process that it forks,
took long enough for 1.5 MB of the 3 MB that the test wrote to arrive
during it, so its base size was 1.5 MB, and the rest of the test didn't
double it: Valkey rewrote once. Shoal's rewrites were short, its base
size stayed small (196 KB at the end), and it rewrote at least four
times. The test checks only what both promise: a rewrite happened, the
log ended smaller than what was written to it, and no rewrite is due.
