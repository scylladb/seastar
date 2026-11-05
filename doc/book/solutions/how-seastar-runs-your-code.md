# How Seastar runs your code {#sol:how-seastar-runs-your-code}

## Exercise 1

The solution is a patch to Shoal version 2,
`examples/solutions/shoal-naive-keys.patch`, whose `KEYS` is the chapter's
second attempt, the loop that keeps an iterator across
`coroutine::maybe_yield()`:

```{.cpp include="examples/solutions/shoal-naive-keys.patch" file="commands.cc" snippet="keys"}
```

All the other tests pass, and `./run 2 -k keys_while_changing` fails in
some runs, not all: in five runs, it failed twice. Once it failed like
this:

```
>               assert len(found) == len(set(found))
E               AssertionError: assert 1910 == 1000
```

`KEYS` returned 1,910 keys, where there were 1,000 distinct ones, so it
listed some keys twice. And once like this:

```
>               assert set(found) == stable
E               AssertionError: assert {'prefix-5c68...able-11', ...} == {'prefix-5c68...ble-102', ...}
```

with no duplicates, but with keys missing. (The test also passes in a
`release` build when neither problem happens to strike, which is the
trouble with such bugs: a test that passes once proves little.)

Duplicates can only come from a rehash. While `KEYS` yielded, the other
client added keys, the table grew, and every key moved to a new bucket;
the iterator, which survived, then went on in the new order, through
keys that it had already listed. Missing keys can come from a rehash
too, from the part of the new order that the iterator had already
passed. The other problem, `DEL` deleting the key that the iterator
points to, makes `++it` read memory that was freed, and its failures are
the unpredictable kind: in a `release` build, freed memory usually still
holds what it held, so `++it` usually still works, and a wrong answer
or a crash would come only when the memory had been reused. So from these
failures, we can tell that rehashing happened, but not whether the
deleted keys did any harm. An AddressSanitizer build would tell: it
reports the use of freed memory at once, as
[chapter](#ch:object-lifetime) shows.

## Exercise 2

`preemption --work maybe_yield`, pinned to CPU 0, in three runs:

| Task quota | Heartbeats | The most delayed |
|---|---|---|
| `--task-quota-ms 0.1` | 910–911 | 0.10–0.31 ms late |
| the default, 0.5 ms | 668 | 0.51–0.54 ms late |
| `--task-quota-ms 5` | 201 | 4.64–4.86 ms late |

The heartbeat wants to run every millisecond, but while the computation
runs, it can run only when `coroutine::maybe_yield()` yields, which it
does when the task quota is used up. So a beat that comes due in the
middle of a quota waits for the quota to end: it's late by up to a
quota, as the table shows. And the next beat comes due a millisecond
after this one ran, so the beats come once every millisecond plus the
wait: about every 1.1 milliseconds with a quota of 0.1, 910 a second;
every 1.5 with the default, 668 a second; and with a quota of 5
milliseconds, longer than the heartbeat's period, once a quota, 200 a
second. The quota is, in effect, the worst latency that any other work on
the shard can see while the computation runs.

The other side of the compromise: Shoal version 2, benchmarked as in
[chapter](#ch:coroutines) (`-t set,get -n 150000000 -P 128 -c 96
--threads 2`, Shoal on CPU 0, the client on CPUs 1 to 3), the best of
three runs:

| Task quota | `SET`s per second | `GET`s per second |
|---|---|---|
| the default, 0.5 ms | 3,660,000 | 4,360,000 |
| `--task-quota-ms 0.05` | 3,440,000 | 4,100,000 |

A quota of 50 microseconds costs about 6% of the throughput. When the
quota runs out, the reactor stops running tasks, and polls: it checks the
network for new data, sends what the streams have buffered, and checks
the timers, which costs a few microseconds even when there's nothing to
find. With a tenth of the quota, it does all this ten times as often; and
since it flushes the streams at every poll, it may also send replies in
smaller batches, with more system calls for the same work (exercise 3 of
[chapter](#ch:coroutines) showed how to count them).
A busy server, whose tasks are short anyway, loses more to the extra
polling than its requests gain in latency, which is why the default is
half a millisecond, not less.

## Exercise 3

With Shoal on CPU 0, its CPU time, which Linux counts in ticks of 10
milliseconds, from `/proc` rather than `top`, so as to get exact numbers,
in three runs:

| | With idle polling (the default) | `--idle-poll-time-us 0` |
|---|---|---|
| idle, for 5 seconds | 2–3 ticks | 0–1 ticks |
| 5,000 `PING`s, one every millisecond | 114–115 ticks, in 5.7 s | 10–15 ticks, in 5.6–5.8 s |

An idle Shoal uses almost no CPU either way: with no requests, the
reactor soon gives up polling and goes to sleep in the kernel until
something arrives. But with a `PING` every millisecond, the default costs
about 20% of a core, 1.14 seconds of CPU time for 5,000 `PING`s, which is
about 230 microseconds for each; without idle polling, about 25
microseconds for each, a tenth as much.

Answering a `PING` takes Shoal a few microseconds at most: reading it,
replying, and the system calls around them. The rest of the 230
microseconds is the reactor's idle polling. After each request, the
reactor doesn't go to sleep at once: it keeps polling, for up to 200
microseconds, `--idle-poll-time-us`'s default, in case another request
comes soon, because waking up from a sleep in the kernel costs tens of
microseconds of latency. With a request every millisecond, Shoal spends
200 microseconds of every millisecond polling for one that doesn't come.
`--idle-poll-time-us 0` makes the reactor sleep as soon as it has nothing
to do, which saves the CPU, and costs each request the wakeup. That's
the right setting for a machine that the server shares with other
programs, or for a server whose load is light, and it's part of what
`--overprovisioned` sets; on a dedicated, busy
server, where requests come all the time, the reactor rarely goes idle,
and idle polling costs nothing.

## Exercise 4

With 4 GB, `--blocked-reactor-notify-ms 2` and up to a thousand reports a
minute, loading about 3.8 million keys made the stall detector report
stalls in `SET`, whose backtraces, through `addr2line`, ended in
`std::unordered_map::insert_or_assign()`, `_M_insert_unique_node()` and
`_M_rehash()`: the table growing. The longest of them grew with the
table, about doubling each time: about 37, 78 and 150 milliseconds for
the last three growths. Each long stall was reported several times, at
the detector's doubling intervals, so the length of a stall is its last
report's. There were also shorter stalls, of 2 to 20 milliseconds, in
`small_pool::add_more_objects()`, inside Seastar's allocator: the first
use of fresh memory, which the kernel must provide, not the hash table.

The growth stalls double because a `std::unordered_map` grows all at
once: when it has more keys than buckets, it allocates a new array of
buckets, about twice as large, and moves every key into it, in one call.
The more keys, the longer the move, and with each doubling, the stall
doubles too. They're rare, once per doubling, but each is as long as the
whole table makes it, and with a few million keys, longer than any task
quota.

A hash table that grows without stalling moves its keys gradually. Redis
does it this way: when its table must grow, it allocates the new one,
and keeps both, and every operation on the table moves a few buckets from
the old one to the new, while lookups look in both, until the old one is
empty. The cost of growing is spread over many operations, and none of
them is long. In Seastar, the same idea can also be expressed with
yielding: a table whose growth is a coroutine that moves a bucket at a
time and calls `coroutine::maybe_yield()`, with lookups that check both
tables while it runs. Either way, a table that must never stall can't be
a `std::unordered_map`, whose rehashing can't be divided.
[Chapter](#ch:memory) returns to the data structures of large Seastar
programs.

## Exercise 5

The solution is a patch to Shoal version 2,
`examples/solutions/shoal-flushall-async.patch`, whose `FLUSHALL ASYNC` hands
the old data to a coroutine that destroys it in the background:

```{.cpp include="examples/solutions/shoal-flushall-async.patch" file="commands.cc" snippet="destroy-gently"}
```

```{.cpp include="examples/solutions/shoal-flushall-async.patch" file="commands.cc" snippet="flushall"}
```

It passes all of version 2's tests, and one more, which checks that a key
set right after `FLUSHALL ASYNC` survives, against Valkey too:

```{.python include="examples/solutions/shoal-flushall-async.patch" file="test_solutions.py"}
```

The hint points at the one thing to be careful about: who owns `old`.
It's a local variable of `flushall()`, which returns, and destroys `old`,
as soon as it has replied, long before the background work is done. So
the background coroutine must not use `old` through a reference: calling
`clear_gently(old)` in the background would leave it erasing elements of
a map that no longer exists. `destroy_gently()` takes the data by value,
and `std::move(old)` moves it into the coroutine's parameter, which lives
in the coroutine's frame, so the coroutine owns the data, for as long as
it runs. A lambda coroutine that captured `old` would have the same
problem as a reference, since its captures are destroyed with the
lambda, as [chapter](#ch:object-lifetime) explains; a parameter taken by
value is the simple, safe way.

Two more things to be careful about don't show in a test. Nothing waits
for the background coroutine, so if `destroy_gently()` could fail, its
failure would go unnoticed, except for Seastar's warning about an
ignored exceptional future; here it can't fail, since erasing elements
doesn't throw. And nothing waits for it when the program stops, so a
clean shutdown would have to. [Chapter](#ch:concurrency-within-a-shard)
builds exactly this `FLUSHALL ASYNC` into Shoal's version 4, and
discusses both, and [chapter](#ch:clean-shutdown) makes a clean shutdown
wait for it.
