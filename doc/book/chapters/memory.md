# Memory {#ch:memory}

A Seastar program allocates memory all the time: a coroutine's frame, a
continuation, a string, a buffer. Earlier chapters said that this is
cheap, because each shard has an allocator of its own, and
[chapter](#ch:shared-nothing) gave the rules that follow from that: a
shard's memory is its own, an object should be freed on the shard that
allocated it, and memory that is free on one shard doesn't help another.
This chapter looks inside the allocator: how it divides the memory, how
it serves small and large allocations, and what that means for a
program's design, such as why a shard with a gigabyte free can't
allocate 900 megabytes, and why a Seastar server must not count on
surviving `std::bad_alloc`. Then come the tools: measuring a shard's
memory, finding out where it went, and giving it back when the allocator
runs low.

Shoal's version 12 applies the lessons to its own data. It replaces
`std::unordered_map` with a table that never makes a large allocation or
stalls the shard as it grows, and that is laid out for the processor's
caches, which makes Shoal more than twice as fast with ten million keys,
and faster than Valkey. And it adds Redis's `--maxmemory`, which evicts
keys to keep the data within a limit.

## A shard's memory {index="memory!per shard"}

When a Seastar program starts, it takes the memory that `-m` (in full,
`--memory`) gives it, or by default, all of the machine's memory except a
reserve for the operating system and other programs, of 1.5 GB or 7% of
the memory, whichever is larger, which `--reserve-memory` changes. It
divides this memory equally among the shards, rounding each shard's part
down to a multiple of 2 MB, and gives each shard a range of addresses of
its own. On a machine with several NUMA nodes, groups of cores that each
have memory of their own, which they reach faster than the others',
Seastar places each shard's memory on its core's node. It asks the
kernel to back the memory with huge pages, of 2 MB rather than 4 KB, so
that the processor needs fewer entries in its TLB, the cache of address
translations, to cover it. And it never gives memory back to the
operating system: memory that a shard frees stays in that shard, for its
next allocation. So a Seastar process grows to its peak and stays there,
which is right for a server that has the machine to itself, and is why
`-m` matters on a machine that it shares.

Seastar replaces `malloc()`, `free()`, `operator new` and
`operator delete` for the whole program, so everything that a shard
allocates comes from its own memory: Seastar's objects and the
program's, `std::vector` and `std::string`, and the memory of
third-party libraries too. Threads that aren't Seastar's, such as a
`std::thread` that the program starts or the threads of
[chapter](#ch:threads), get the C library's `malloc()` instead, and so
does code that runs before Seastar starts, such as the constructors of
global objects. Memory can be freed on either side: Seastar recognizes
its own memory by its address, and hands anything else to the C
library's `free()`.

As [chapter](#ch:shared-nothing) showed,
[`memory::stats()`]{index="`seastar::memory::stats()`"} describes the
calling shard's memory. This function prints two of its numbers:

```{.cpp include="examples/memory/allocations.cc" snippet="show"}
```

Run on one shard with a gigabyte, `-c1 -m1G`, it prints:

```
at the start: 1024 MB in all, 1004 MB free
```

The 20 MB that are already taken are mostly the allocator's own: it
keeps 32 bytes about each 4 KB page of the shard, which page is free,
and what each page holds.

## Small allocations {index="memory allocator!small allocations"}

An allocation of up to 16 KB is *small*. Each shard has a pool for each
of about forty sizes, from 8 bytes up, with four sizes for each doubling
above 64 bytes (80, 96, 112, 128, 160, and so on), and an allocation takes
an object from the pool of the smallest size that fits, so it wastes at
most a quarter of what it asked for. A pool keeps its free objects in a
list, linked through the objects themselves: allocating takes the first,
and freeing puts one back, in a few instructions, with no lock and no
atomic operation, since no other thread allocates from this shard's
pools. An object has no header: the allocator finds an object's pool
from its address, through its table of pages, so an object of 64 bytes
takes exactly 64 bytes. A pool gets its objects by taking a run of pages
from the allocator of large allocations, below, and cutting it into
objects, and returns the run only when every object in it is free.

This is why [chapter](#ch:coroutines) could call a coroutine's frame
cheap: allocating it, and freeing it when the coroutine ends, are a few
instructions each.

## Large allocations {index="memory allocator!large allocations"}

An allocation of more than 16 KB takes whole pages, from a *buddy
allocator*[]{index="buddy allocator"}: the free pages are kept in
blocks whose sizes are powers of two, each aligned to its own size. An
allocation is rounded up to the next power of two, and takes a free
block of that size, splitting a larger one in halves, and the halves in
halves, if it must. When a block is freed, the allocator looks at its
*buddy*, the other half of the block that it was split from, whose
address it computes from the freed block's own, and if the buddy is free
too, merges the two, and then the merged block with its own buddy, and so
on. Splitting and merging take a few steps each, which is why Seastar's
allocator works this way. The cost is the rounding:

```{.cpp include="examples/memory/allocations.cc" snippet="rounding"}
```

This prints `600 KB took 1024 KB`. (`escape()`, not shown, keeps the
compiler from removing an allocation that nothing reads.) An allocation
of 600 KB took a block of a megabyte, and wasted 40% of it.

A large allocation also needs its whole block in one piece, however much
memory is free in all:

```{.cpp include="examples/memory/allocations.cc" snippet="too-big"}
```

On a shard of a gigabyte, with 1,004 MB free, this prints:

```
900 MB: std::bad_alloc
600 MB: std::bad_alloc
500 MB: allocated
```

Both 900 MB and 600 MB are rounded up to a block of 1 GB, the whole
shard, whose first pages hold the allocator's own table. The shard's top
half is free, as one block of 512 MB, so 500 MB fits. On a shard of a
gigabyte, no allocation can be larger than 512 MB, which is the result
that exercise 4 of [chapter](#ch:shared-nothing) found surprising. And it
gets worse as the shard is used. After a while, its free memory is
scattered, in blocks between allocated ones, and the largest free block
may be much smaller than the free memory. The allocator can't move
allocated blocks to make room, since the program holds pointers to them,
so a large allocation can fail on a shard that has plenty of memory
free. This is *fragmentation*[]{index="fragmentation"}.

So a server that runs for months should avoid large contiguous
allocations: a `std::vector` that grows to megabytes, a large string, or
the bucket array of a hash table, which is about 8 MB for a million keys
in `std::unordered_map`. It should use structures made of many small
pieces instead.
[`chunked_fifo`]{index="`seastar::chunked_fifo`" .main}, from
`<seastar/core/chunked_fifo.hh>`, is a queue made of chunks of 128
elements each, so it never makes a large allocation, however long it
gets; [`circular_buffer`]{index="`seastar::circular_buffer`"}, which
Seastar uses for many of its own queues, is one array that doubles as it
grows, as a `std::vector` does, so it suits only queues that stay
short. ScyllaDB has a `chunked_vector` and fragmented buffers for the
same reason, and Redpanda has its `iobuf`, a buffer made of fragments.
Shoal's version 12, below, does the same for its keys.

To find a program's large allocations, Seastar can log a warning, with a
backtrace, for each allocation larger than a threshold:

```{.cpp include="examples/memory/allocations.cc" snippet="warning"}
```

which logs:

```
WARN  [shard 0:main] seastar_memory - oversized allocation: 1048576 bytes.
This is non-fatal, but could lead to latency and/or fragmentation issues.
Please report: at 0x585b28 0x5863e6 ...
```

The warning is off by default, and
[`memory::set_large_allocation_warning_threshold()`]{index="`seastar::memory::set_large_allocation_warning_threshold()`" .main},
from `<seastar/core/memory.hh>`, turns it on for the calling shard only,
so a program that wants it on every shard must call it on every shard.
[`memory::scoped_large_allocation_warning_threshold`]{index="`seastar::memory::scoped_large_allocation_warning_threshold`"}
changes the threshold until it's destroyed, for code that is allowed
larger allocations. After each warning, the threshold rises for a while,
so that a program that makes many large allocations doesn't flood its
log. ScyllaDB, by default, warns about every allocation over 128 KB.

## Freeing on another shard {index="memory allocator!cross-shard free"}

As [chapter](#ch:shared-nothing) said, an object should be destroyed on
the shard that created it, with `foreign_ptr` carrying it home. But
memory that another shard frees, such as the characters of a string that
a reply carried from shard to shard, must still go back to the shard it
came from, and it does. An object's address tells which shard owns it,
and the shard that frees it puts it in a list that belongs to the owner,
with one atomic compare-and-swap. The owner's reactor takes the whole
list, in each pass of its loop, and frees the objects for real. This
program measures what a free costs, when the owner frees its own
objects, and when other shards free them, one at a time and several at
once:

```{.cpp include="examples/memory/cross-free.cc" snippet="compare"}
```

With four shards, on the laptop, in the best of three runs:

| Who frees shard 0's objects | Time for each free |
|---|---|
| Shard 0, their owner | 11.5 ns |
| One other shard | 9.5 ns |
| Two other shards at once | 73 ns |
| Three other shards at once | 102 ns |

A free by one other shard is no slower than the owner's own, since it
only puts the object in a list, and the owner pays for the rest later,
which this measurement doesn't count. But several shards at once compete
for the one cache line that holds the list's head: each compare-and-swap
needs the line for itself, and the line moves from core to core, so each
free costs eight to eleven times as much. A few objects freed on the wrong
shard cost nothing worth worrying about, but a stream of them, from many
shards to one, does. And the memory comes back to its owner only when
the owner's reactor runs, so an idle shard may hold memory that others
freed until something wakes it.

## Watching memory {index="memory!statistics"}

`memory::stats()` returns a
[`memory::statistics`]{index="`seastar::memory::statistics`" .main},
from `<seastar/core/memory.hh>`, with more than the two numbers above:
`total_memory()`, `free_memory()` and `allocated_memory()`; the counts
of allocations and frees, `mallocs()`, `frees()` and
`cross_cpu_frees()`, and the objects still alive, `live_objects()`; and
`large_allocations()`, the warnings above, and `failed_allocations()`.
`free_memory()` counts only free pages: a free object in a pool doesn't
count, since its page isn't free. Seastar also exports these numbers as
metrics, which [chapter](#ch:observability) shows how to watch. Since a
Seastar program runs out of memory when one of its shards does, the
number to watch is the free memory of the fullest shard, not the total.

Finding *where* the memory went is harder. Seastar can sample
allocations: one allocation in so many bytes, chosen at random, records
its backtrace, and
[`memory::sampled_memory_profile()`]{index="`seastar::memory::sampled_memory_profile()`" .main},
from `<seastar/core/memory.hh>`, returns, for each backtrace that
allocated memory still in use, how many objects and about how many
bytes. The sampling must be compiled into Seastar, with `configure.py
--heap-profiling` ([appendix](#app:building-seastar)), and turned on with
`--heapprof`, which sets the average number of bytes between samples.
[Chapter](#ch:memory-leaks) uses it to find memory leaks.

## Running out {index="`std::bad_alloc`"}

When a shard can't find memory for an allocation, it first asks its
reclaimers, below, to give some back, and if they can't, the allocation
fails: `operator new` throws `std::bad_alloc`, and `malloc()` returns a
null pointer. With `--abort-on-seastar-bad-alloc`, the program aborts
instead, at once, and its core dump shows where it was, which helps in
tests. And with `--dump-memory-diagnostics-on-alloc-failure-kind=all`,
Seastar logs a report on each failure (at most once in ten seconds):
the shard's memory, its pools, and a table of its free blocks by size,
which is the only view of fragmentation that Seastar gives.

Can a server catch `std::bad_alloc`, give up the request that failed,
and go on? Its own code can be written for that: with RAII, an exception
from an allocation is like any other. But Seastar itself allocates
memory in places where it can't report a failure, for example when it
schedules a continuation. Its source says so:

```
// If this new throws a std::bad_alloc there is nothing that
// can be done about it. The corresponding future is not ready
// and we cannot break the chain. Since this function is
// noexcept, it will call std::terminate if new throws.
```

The same holds for sending a message to another shard, and for
`yield()`. So a Seastar server that runs out of memory may end, however
careful its own code is. The defense is not to get there: bound
everything that can grow. [Chapter](#ch:coordination) bounded the memory
of Shoal's requests, and [chapter](#ch:networking) the memory of its
replies, with semaphores of bytes; caches can
give memory back, below; and large allocations, which fail first, should
be avoided anyway.

Seastar can also test the code that handles a failed allocation.
[`memory::with_allocation_failures()`]{index="`seastar::memory::with_allocation_failures()`" .main},
from `<seastar/util/alloc_failure_injector.hh>`, runs a function again
and again, making its first allocation fail, then its second, and so on,
until it runs to the end without a failure; a test checks that each run
leaves everything consistent. The failures are only injected in Seastar's
`dev` builds, or in builds configured with `--enable-alloc-failure-injector`
([appendix](#app:building-seastar)).

### In debug builds {index="memory allocator!in debug builds"}

Seastar's `debug` and `sanitize` builds don't use its allocator at all,
but the C library's, so that AddressSanitizer can check every
allocation. Then `-m` limits nothing, `memory::stats()` returns made-up
numbers (a gigabyte, always free), reclaimers are never called, and
large-allocation warnings and heap profiling do nothing. So whatever
depends on memory running low can only be tested in `release` and `dev`
builds.

## Giving memory back: reclaimers {index="reclaimer"}

A cache should use the memory that nothing else needs, and give it back
when something does. Seastar's allocator can ask for it: a
[`memory::reclaimer`]{index="`seastar::memory::reclaimer`" .main}, from
`<seastar/core/memory.hh>`, registers a function, which the allocator
calls when the shard's free memory falls below a low-water mark, about
19 MB unless the program changes it with `memory::set_min_free_pages()`,
or when a large allocation finds no block. The function is asked to free
so many bytes, and says whether it freed anything. Seastar's
documentation leaves this interface out, as internal, though the header
has it and it's exported to applications: ScyllaDB's cache is built on
it. A program that uses it should expect it to change, without the
deprecation period that Seastar gives its public interfaces, and check it
at every upgrade ([chapter](#ch:upgrading-seastar)).

This cache keeps blobs of a megabyte, and drops the oldest when the
allocator asks:

```{.cpp include="examples/memory/reclaimer.cc" snippet="cache"}
```

`reclaimer_scope::async` means that the allocator never calls the
function itself, inside an allocation, not even one that is about to
fail, but schedules it, as a task at the front of the default scheduling
group's queue, which runs as soon as the current task ends. A reclaimer
of `reclaimer_scope::sync` may be called inside an allocation, so it must
be quick, and shouldn't allocate memory. The program adds 3,000 blobs to a shard of a gigabyte, letting
other tasks run after each:

```{.cpp include="examples/memory/reclaimer.cc" snippet="fill"}
```

It prints `985 blobs kept, 2015 dropped, 19 MB free`: the cache kept
what fit above the low-water mark. Without the reclaimer, the program
fails with `std::bad_alloc` after about a thousand blobs, and without the
`yield()`, it fails too, since the reclaimer, which waits for its turn
as a task, never gets one.

A reclaimer must free *pages*, and here lies a trap. The allocator
counts free memory in whole pages, and a page of small objects becomes
free only when every object on it is. Small objects that a cache frees
go back to their pool, where its next objects of the same size reuse
them, but they don't help anything that needs pages: a large
allocation, or objects of another size. And a cache that evicts small
objects in an order unrelated to where they lie frees few whole pages.
We filled 900 MB of a shard with objects of 1 KB, and freed half of
them, 450 MB: freed in the order in which they were allocated, they gave
back 449 MB of free pages, but freed at random, only 56 MB. The blobs
above are large allocations, each with pages of its own, so dropping one
frees its pages at once. ScyllaDB solves this for its cache with an
allocator of its own, built on top of Seastar's, the *log-structured
allocator*, whose objects can be moved: it packs the objects that remain
into fewer pages, and gives the pages it emptied back to Seastar.
(ScyllaDB's code is under a source-available license, not an open-source
one, but the idea is free to reuse.) Shoal's `--maxmemory`, below,
avoids the trap differently.

## Shoal, version 12 {index="Shoal!version 12"}

Version 12 keeps the promises of three earlier chapters:
[chapter](#ch:how-seastar-runs-your-code)'s, that `KEYS` wouldn't need
to start over when the table grows, and would look at the keys as
quickly as Valkey does; [chapter](#ch:time)'s, that the table would
neither stall the shard as it grows, nor need a large allocation; and
[chapter](#ch:designing-a-sharded-application)'s, that Shoal would close
the gap to Valkey with ten million keys. One table does all three.

### Why ten million keys are slow

[Chapter](#ch:designing-a-sharded-application) measured that Shoal's
version 8 took 3,098 cycles for a `GET` of a random key out of ten
million, but only 859 for a `GET` of one key, with the same instructions. The difference
was 4.6 loads from main memory for each `GET`, about 490 cycles each,
which came one after another. A lookup in `std::unordered_map` loads the
key's bucket, which points to a node of the table's list; that node,
which points to the next; the key's characters, which the 16-character
keys of `valkey-benchmark` keep in an allocation of their own, since an
`sstring` keeps only 14 inside itself; and the value. Each load needs
the address that the one before found, so the processor can't start
them together.

There are two cures: fewer loads, and loads that overlap. Version 12
uses both.

### A table of small tables {index="hash table!of small tables"}

A key keeps up to 22 characters inside itself, rather than 14:

```{.cpp include="shoal/v12/table.hh" snippet="key-string"}
```

The table is made of many small hash tables, each with a few thousand
keys at most. The first bits of a key's hash choose its small table,
through a *directory*, as in the *extendible hashing* of database
systems:

```{.cpp include="shoal/v12/table.hh" snippet="key-table"}
```

A small table is an array of slots, each with a key's hash and a pointer
to the key's *node*, which holds the key and its value. A lookup goes
from the slot that the hash's low bits choose to the next ones, until it
finds the key or an empty slot (*open addressing*):

```{.cpp include="shoal/v12/table.hh" snippet="probe"}
```

```{.cpp include="shoal/v12/table.hh" snippet="find"}
```

The directory and the small tables' headers are small, and stay in the
processor's cache, so a lookup loads two things from main memory: a
slot, and a node. The slot holds the key's full hash, so a lookup
compares keys only when their hashes are equal, which almost always
means that the key is found. And the node is a single cache line:

```{.cpp include="shoal/v12/commands.hh" snippet="entry"}
```

With its key, an `entry` takes 64 bytes, which the allocator serves from
its pool of 64-byte objects, each in a cache line of its own. Getting
there taught us how much this matters. Adding `--maxmemory`, below,
added two 4-byte fields to `entry`, which then took 136 bytes with its
key, rather than 128. The allocator rounded that up to 160 bytes, and
since 160 isn't a multiple of 64, many of its 160-byte objects don't
start at the beginning of a cache line, so a node now often spread over
three cache lines rather than two, and the field that every lookup
wrote, at the end, was in the third, which Shoal's prefetching, below,
didn't load in advance: it then loaded two cache lines of each node.
With random keys, every request got 6% to 7% slower, from 8 bytes. Shrinking the node to 64 bytes, by
keeping keys of up to 22 characters inside rather than 30, putting the
small fields first, and keeping a list's elements behind a pointer, made
Shoal faster than before the 8 bytes.

A key that isn't there is added to its small table. When a small table is
three quarters full, it grows: it doubles its slots, and moves the keys'
slots to the new array, but not the nodes, which stay where they are.
When it has 4,096 slots, it splits in two instead, by the next bit of
the hash, and the directory doubles if it must:

```{.cpp include="shoal/v12/table.hh" snippet="try-emplace"}
```

```{.cpp include="shoal/v12/table.hh" snippet="split"}
```

So growing the table never moves more than one small table's slots, a
few thousand, which takes microseconds, and a small table's slots never
take more than 64 KB, however many keys there are: no stalls, and no
large allocations. Only the directory is one array, of 4 bytes for each
of its entries, a few for each small table, so it stays small too.

### Prefetching {index="prefetching"}

Two loads that each wait 490 cycles are still slow. But a pipelined
client sends many requests at once, and Shoal's reader
([chapter](#ch:networking)) parses all those that have arrived before
Shoal executes the first, so Shoal knows the next keys early, and can
ask the processor to start loading what their lookups will need. The
processor loads it in the background, while it executes the requests
before, and when the lookup comes, the data is in the cache. Seastar's
[`prefetch()`]{index="`seastar::prefetch()`" .main}, from
`<seastar/core/prefetch.hh>`, does this for a pointer: it issues the
processor's prefetch instruction for each cache line of the object. A
prefetch never fails, even for an address that isn't valid, and doesn't
wait for anything. The table prefetches in two steps:

```{.cpp include="shoal/v12/table.hh" snippet="prefetch"}
```

The slot's address comes from the hash alone, but the node's address is
in the slot, so the node can be prefetched only once the slot has
arrived. Shoal prefetches the slot of the request eight ahead, and the
node of the request four ahead, whose slot it prefetched four requests
earlier:

```{.cpp include="shoal/v12/main.cc" snippet="prefetch-ahead"}
```

With more than one shard, most requests are executed on other shards,
in batches, and the shard that executes a batch prefetches its keys the
same way, before executing it. Prefetching isn't free: it hashes each
key twice more. With one key, which is always in the cache, it only
costs: on one shard, too little to measure, but on several, where most
requests arrive in batches, up to 10%.

`perf stat` shows what the table and the prefetching changed, for each
`GET` of a random key out of ten million, on one shard:

| For each `GET` | Shoal 8 | Shoal 12, without prefetching | Shoal 12 |
|---|---|---|---|
| Instructions | 2,935 | 2,997 | 3,046 |
| Cycles | 3,098 | 1,704 | 969 |
| Loads from main memory | 4.6 | 1.8 | 0.4 |

The instructions hardly changed, but the cycles fell to less than a
third. The table cut the loads from main memory to fewer than two, and
the prefetching left fewer than half a load for the lookup to wait for:
a `GET` of a random key now costs only about 260 cycles more than a `GET`
of a single key, 710.

### `KEYS` that never starts over {index="`KEYS`!in version 12"}

[Chapter](#ch:how-seastar-runs-your-code)'s `KEYS` yielded between
buckets of `std::unordered_map`, and had to start over when the table
grew, since a rehash moved every key. Version 12's `KEYS` goes through
the small tables one at a time:

```{.cpp include="shoal/v12/table.hh" snippet="scan"}
```

A position is a hash, the first of a small table's range of hashes, and
`scan()` returns the first hash of the next range. Growing a small table
doesn't change its range. Splitting one divides its range in two: if
the table is ahead of the scan, the scan visits both halves; if it's
behind, the scan has visited all its keys already. So a scan that yields
between small tables, while other clients add and delete keys, visits
each key that is there all along exactly once, and never starts over.
And it's fast: it walks each small table's slots in the order they lie
in memory, and prefetches the node of the slot eight ahead. With
[chapter](#ch:how-seastar-runs-your-code)'s 950,000 keys, a `KEYS` that
matches nothing took 15.6 milliseconds, where version 8 took 63.4, and
Valkey 18.9, without yielding.

### Evicting keys: `--maxmemory` {index="`--maxmemory`"}

Redis can keep its data within a limit, `--maxmemory`, by evicting keys
when it's over: with `--maxmemory-policy allkeys-lru`, the keys used
least recently. Shoal's version 12 does the same, on each shard, with
the shard's part of the limit:

```{.cpp include="shoal/v12/commands.hh" snippet="store-memory"}
```

A reclaimer would be one way: evict keys when Seastar's allocator runs
low. But `--maxmemory` is a limit that the user chooses, as in Redis,
usually well below the memory that Shoal has, which leaves the rest for
connections, requests and replies; a reclaimer is called only when the
shard is nearly full. And Shoal's keys and values are small objects of
many sizes, so evicting them would give back few pages, as the trap
above showed, to the buffers that need them. So Shoal counts the memory
of its keys itself, as Redis does, with an estimate for each key:

```{.cpp include="shoal/v12/commands.cc" snippet="footprint"}
```

Every change is recorded in the log, with its key, so `record()` calls
`account()`, and the count is right after every command. The estimate is
close: a table of 950,000 keys with short values took 101 bytes for
each, by `memory::stats()`. The estimate must also be cheap: the first
version added up a list's elements each time, which made each `RPUSH`
to a long list slower than the one before, so a list counts its bytes as
elements come and go.

When the shard is over its limit, the next command evicts keys first:

```{.cpp include="shoal/v12/commands.cc" snippet="evict"}
```

Like Redis, Shoal doesn't keep the keys in order of use, which would
cost memory and time on every lookup, but samples five keys at random
and evicts the one used least recently, an approximation that is good
enough for a cache. Each lookup records the time in the key's entry.
`sample()` chooses a random small table, and in it, the first key from a
random slot on:

```{.cpp include="shoal/v12/table.hh" snippet="sample"}
```

The tests check that writing far more than the limit keeps the memory
near the limit, and keeps the keys written last, and a key read again and
again, as they do for Valkey. Valkey's clock for this ticks once a
second, so the tests wait a second between the keys they expect to keep
and the others.

### What it bought

As in [chapter](#ch:designing-a-sharded-application), we measured with
`valkey-benchmark`: 40 million `SET`s, and then `GET`s for half a
minute, in pipelines of 64 requests, on 64 connections, with random keys out of ten
million, and with a single key. Version 12 ran without its log, as
Valkey did, and its numbers are the best of three runs, in millions of
requests a second; version 8's and Valkey's are
[chapter](#ch:designing-a-sharded-application)'s:

| Server | Cores | `SET`, random keys | `GET`, random keys | `SET`, one key | `GET`, one key |
|---|---|---|---|---|---|
| Shoal 8 | 1 | 1.26 | 1.28 | 3.35 | 3.75 |
| Shoal 8 | 2 | 2.23 | 2.28 | 5.26 | 5.79 |
| Shoal 8 | 3 | 3.10 | 3.38 | 6.79 | 7.21 |
| Shoal 8 | 4 | 4.15 | 4.36 | 8.51 | 8.45 |
| Shoal 12 | 1 | 3.12 | 3.40 | 4.07 | 4.56 |
| Shoal 12 | 2 | 4.17 | 4.43 | 5.78 | 6.32 |
| Shoal 12 | 3 | 5.96 | 6.00 | 6.99 | 7.27 |
| Shoal 12 | 4 | 7.76 | 7.23 | 7.82 | 8.39 |
| Valkey | 1 | 1.96 | 2.98 | 2.55 | 3.31 |
| Valkey | 2 | 2.49 | 4.27 | 3.59 | 5.25 |
| Valkey | 3 | 2.34 | 3.86 | 3.21 | 4.50 |
| Valkey | 4 | 2.24 | 3.61 | 3.05 | 4.23 |

With random keys, one shard of version 12 does 2.5 times as many `SET`s
as version 8, and 2.7 times as many `GET`s, and more than Valkey's one
thread: 3.12 million `SET`s against 1.96, and 3.40 million `GET`s
against 2.98. On four shards, it does 7.76 million `SET`s and 7.23
million `GET`s, 1.7 to 1.9 times as many as version 8, and 1.7 times as
many `GET`s as Valkey at its best.

But version 12 gains less from each added shard than version 8 did:
with random keys, two shards do 1.3 times as many `GET`s as one, where
version 8's did 1.8 times as many.
[Chapter](#ch:designing-a-sharded-application) modeled this with a
fraction $f$, how much more a request costs when it's forwarded to
another shard than when it's executed on its connection's own, and
fitted it to random keys, $f = 0.25$. Fitted the same way to one key, from
the table above, $f$ was 0.59 for version 8, and is 0.89 for version 12. In
nanoseconds, a forwarded `GET` costs 158 more than a local one in version
8, and 195 more in version 12, which also prefetches the keys of the
batches it forwards. But even version 8's 158 nanoseconds would be a
larger fraction of version 12's requests, 0.72, because they got
cheaper: a faster shard makes the same overhead weigh more, so the
scaling looks worse while every shard got faster. With random keys,
forwarded requests also gain less from prefetching, since a batch of
them is often short, and its first requests have no time to prefetch
for.

## Exercises

1. Add Redis's `SCAN cursor`, which [chapter](#ch:designing-a-sharded-application)'s
   exercise 1 also asked for, so that its cursor never starts over:
   encode in the cursor a shard and a position from `scan()`. Test it
   while another client adds and deletes keys, and check that every key
   that exists throughout is returned exactly once.

2. In `allocations.cc`, allocate a thousand blocks of a megabyte, and
   then free every other one. How much memory does `memory::stats()` say
   is free? What is the largest allocation that succeeds now? Explain
   both numbers with the buddy allocator.

3. In `reclaimer.cc`, make the blobs 1 KB rather than a megabyte, and add
   two million of them. Does the cache still keep the shard full,
   despite the trap described above? Why? Then make the reclaimer drop
   blobs at random rather than the oldest. What changes?

4. Measure the lesson of version 12's node: add an 8-byte field to
   `entry`, and check with `sizeof` which of the allocator's sizes the
   node now takes. Measure `GET`s of random keys out of ten million,
   before and after, on one shard. Then make the field 16, 24 and 64
   bytes. Does the cost jump, as it did in the chapter's story? Why, or
   why not?

5. Shoal 12 prefetches the slot of the request eight ahead, and the node
   of the request four ahead. Try other distances, such as 2 and 1, 4 and
   2, and 16 and 8, and measure `GET`s of random keys on one shard. Which
   is fastest? To find out why, count how many requests `prefetch_ahead()`
   finds parsed and waiting, each time it's called.
