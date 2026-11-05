# Designing a sharded application {#sol:designing-a-sharded-application}

## Exercise 1

The solution is a patch to Shoal version 8,
`examples/solutions/shoal-scan.patch`. Its tests came first, and pass against
Valkey:

```{.python include="examples/solutions/shoal-scan.patch" file="test_solutions.py"}
```

What they may assume is what Redis promises: every key that exists from
the first call to the last is returned at least once; a key may be
returned more than once; and a key added or deleted during the scan may
be returned or not. So the tests compare sets, and check only the keys
that exist throughout. The first version of the second test added 500
keys after every call, and against Valkey it never ended: each call
returned ten keys, so the keys were added faster than they were listed.
Redis's documentation warns of this: a scan of a collection that keeps
growing may never finish. The test now adds keys only during its first
calls, enough to make the tables grow several times.

The cursor is a number, and Shoal chooses what it means: here, which
shard to continue on, in its lowest 8 bits, which bucket of that shard's
hash table, in the next 32, and the low 24 bits of the number of buckets
the table had, in the highest:

```{.cpp include="examples/solutions/shoal-scan.patch" file="commands.cc" snippet="scan"}
```

with one more line in the table of commands:

```cpp
    {"scan", {.arity = -2, .run = scan}},
```

Each call scans one shard, from the cursor's bucket, until it has found
`COUNT` keys, or reached the end of the shard's table; the next call
continues there, or on the next shard, and after the last shard, the
cursor is 0. A call may return fewer keys than `COUNT`, or none, which
`SCAN` allows: a client must go on until the cursor is 0.

The tricky part is a table that grows between two calls. Its keys then
move to new buckets, and a bucket number from before the growth means
nothing: continuing from it could skip keys that moved to buckets that
the scan had not reached yet, behind the cursor. Redis avoids this with
a clever order of visiting the buckets, which works because its tables
always have a power of two buckets; `std::unordered_map`'s don't. So the
cursor remembers how many buckets the table had, and when the number
changed, the shard is scanned again from its first bucket. Keys that were
already returned are returned again, which `SCAN` allows, and none that
existed all along is missed. With these, all 83 tests pass, against
Valkey and the patched Shoal.

Dividing the keys among shards made the cursor easier, not harder: each
shard's table is scanned on its own, and the shard's number is just one
more part of the cursor, which no client looks inside.

## Exercise 2

The solution is a patch to Shoal version 8,
`examples/solutions/shoal-publish-shards.patch`. The question is where to keep
the set of shards that have subscribers to each channel. On shard 0, a
`PUBLISH` from any other shard would first ask shard 0 for the set, a
hop, before visiting the subscribers' shards, a second one. A copy on
every shard costs `PUBLISH` nothing: it reads its own shard's copy. The
copies cost `SUBSCRIBE`, which must update all of them, but only when a
shard gets its first subscriber to a channel, or loses its last. Clients
publish far more often than they subscribe, so the copy on every shard
is the better choice:

```{.cpp include="examples/solutions/shoal-publish-shards.patch" file="pubsub.hh" snippet="shard-sets"}
```

```{.cpp include="examples/solutions/shoal-publish-shards.patch" file="pubsub.cc" snippet="tell-all"}
```

`SUBSCRIBE` tells every shard before it confirms the subscription, and
waits until all of them know:

```{.cpp include="examples/solutions/shoal-publish-shards.patch" file="pubsub.cc" snippet="subscribe"}
```

That is what makes sure that no message is lost: a `PUBLISH` that
starts after the client got its confirmation, on any shard, finds the
client's shard in its own copy. Losing the last subscriber, in
`remove()`, tells the other shards in the background: a `PUBLISH` that
still visits the shard meanwhile finds no subscribers there, and
reports none, which is right. The gate makes `stop()` wait for these
messages.

The version numbers are there because the chapter's lesson applies here
too: Seastar doesn't promise that messages are handled in the order in
which they were sent. A shard that loses its last subscriber and then
gets a new one sends "no subscribers" and then "subscribers"; if another
shard handled them in the opposite order, it would end up believing that
the shard has no subscribers, and a later `PUBLISH` would skip it, losing
the message. Each shard numbers the changes it tells, and a change older
than one already known is ignored.

`PUBLISH` then visits only the shards in its copy of the set:

```{.cpp include="examples/solutions/shoal-publish-shards.patch" file="pubsub.cc" snippet="publish"}
```

The tests, which pass against Valkey and the patched Shoal, check a `PUBLISH`
right after each of twenty subscriptions, and subscribers who come and
go:

```{.python include="examples/solutions/shoal-publish-shards.patch" file="test_solutions.py"}
```

This program measures `PUBLISH`, with a subscriber connected before the
publisher, so that the two are on different shards:

```{.python include="examples/solutions/designing-a-sharded-application-ex2.py"}
```

On four shards (`--cpuset 0-3`), with the program on the slow cores, in
the best of three runs, version 8 took 4.64 microseconds per `PUBLISH`,
and the patched Shoal 3.51, about a quarter less. Version 8's `PUBLISH` visits
all four shards, three of them through a hop, and waits for all of them;
the patched Shoal's visits only the subscriber's shard, through one hop. Since
the hops are made at the same time, the patched Shoal doesn't save the time of
two round trips; it saves sending two more messages, running
`publish_here()` on two more shards, and adding up their replies. On a
server whose shards are busy with other work, that work is taken from
it, and the more shards, the more `PUBLISH` saves.

## Exercise 3

Without the loop, `test_blpop_several_keys` crashed the `debug` build of
the server, and every test after it failed, for want of a server:

```
==301569==ERROR: AddressSanitizer: heap-use-after-free on address ...
    #2 ... in serve_blocked .../commands.cc:697
    #3 ... in push .../commands.cc:737
    #4 ... in rpush .../commands.cc:742
    ...
freed by thread T3 here:
    #2 ... in blpop .../commands.cc:912
```

Line 697 is `w->done.exchange(true)`, in `serve_blocked()`, on a push;
the memory was the `blpop_wait` in `blpop()`'s frame, freed when
`blpop()` returned, on another shard, whose thread is T3. The test blocks
a client on two keys, `k1` and `k2`, pushes to `k2`, which ends the wait,
and then pushes to `k1`, to check that the element stays in the list.
Without the loop, the client's place in `k1`'s line, on `k1`'s shard,
was never removed, and the push to `k1` found it, and followed its
pointer to the wait, which no longer existed.

Against a `release` build, the same tests passed, three runs out of
three. The freed frame's memory still held what it held, and `done` read
as `true`, so `serve_blocked()` skipped the stale place in line, as it
should have. The bug was there all the same: `exchange()` also *wrote*
`true` to freed memory, which, once the memory is reused for something
else, corrupts that something, somewhere else, later. That's why the
cleanup is needed: a pointer to an object on another shard is safe only
as long as the object lives, and the shard that owns the object must
make sure, before destroying it, that no other shard still has the
pointer. And it's why tests of code that shards share must run in
`debug` mode too.

## Exercise 4

The prediction: with two shards, each owns half of the keys, and serves
about half of the connections, so each does half of the work, and every
connection has requests on both. The slow shard takes longer to do its
half, and the fast one, which has finished its own, waits for it: the
connections, which need both shards' replies, go at the slow shard's
pace. So two shards, one of them slow, should do about twice as much as
the slow core alone, which may be more or less than one fast core.

Measured as in the chapter, with random keys, the best of three runs
(requests a second):

| Shoal version 8 | `SET` | `GET` |
|---|---|---|
| 1 shard, a fast core (`--cpuset 0`) | 1.27 million | 1.29 million |
| 1 shard, a slow core (`--cpuset 4`) | 0.94 million | 0.97 million |
| 2 shards, a fast and a slow core (`--cpuset 0,4`) | 1.86 million | 1.96 million |
| 2 shards, two fast cores (`--cpuset 0-1`) | 2.23 million | 2.28 million |

A fast core and a slow one did almost exactly twice what the slow core
does alone: the two shards went at the slow shard's pace, and the fast
core, which alone does about a third more, spent the difference
waiting. Still, the slow shard was worth having here: the pair did half
as much again as the fast core alone. It
would be worth less with a slower core, and with many fast shards and
one slow one, it could be worth nothing, since all of them would go at
its pace.

Shoal divides the keys equally among the shards, and Seastar the memory,
because they assume that the cores are equal, as they are on the servers
it was designed for. On a machine whose cores aren't, the cure is to use
only the equal ones: on this laptop, the four fast cores, with
`--cpuset 0-3`, as this book's measurements do.
