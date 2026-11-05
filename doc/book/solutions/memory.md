# Memory {#sol:memory}

## Exercise 1

The solution is a patch to Shoal version 12,
`examples/solutions/shoal-scan-resume.patch`, which ports the solution of
[chapter](#ch:designing-a-sharded-application)'s exercise 1 to version
12's table. Its tests are that solution's, which pass against Valkey,
and one more, for what version 12 promises beyond Redis: while many keys
are added, which makes the small tables grow and split again and again,
a scan returns each key that exists throughout exactly once:

```{.python include="examples/solutions/shoal-scan-resume.patch" file="test_solutions.py" snippet="once"}
```

The cursor is simpler than chapter 15's. A position of `scan()` is the
first hash of a small table's range, a multiple of a power of two that
is far larger than 256, so its lowest 8 bits are always 0, and they can
hold the shard:

```{.cpp include="examples/solutions/shoal-scan-resume.patch" file="commands.cc" snippet="scan-here"}
```

The rest, parsing the options and choosing the next shard, is chapter
15's. Chapter 15's cursor had to remember how many buckets the table
had, and start the shard over when the number changed; this one needs
neither, since a position stays right however the table grows. All 109
tests pass, against the patched Shoal, and the four of `SCAN` against
Valkey too.

## Exercise 2

The program allocates the thousand blocks, frees every other one, and
then finds the largest allocation that succeeds, by trying sizes in a
binary search:

```{.cpp include="examples/solutions/memory-ex2.cc" snippet="holes"}
```

On one shard of a gigabyte, it prints:

```
at the start: 1004 MB free, largest allocation 524288 KB
after freeing every other block: 504 MB free, largest allocation 4096 KB
```

At the start, the largest allocation is 512 MB, the shard's free top
half, as the chapter explained. A block of a megabyte is 256 pages, a
power of two, so each takes exactly a block of the buddy allocator, and
blocks allocated one after another lie next to each other, in pairs of
buddies. Freeing every other block frees one of each pair, and a freed
block can merge only with its buddy, which is still allocated. So the
500 MB that are free again are 500 separate blocks of a megabyte, and
the largest allocation that fits is 4 MB, a piece of the memory that the
thousand blocks didn't use. Half the shard is free, and no allocation
larger than a few megabytes can succeed: that is fragmentation, made on
purpose.

## Exercise 3

The program is `reclaimer.cc`, with blobs of 1 KB, and a reclaimer that
drops the oldest blobs, or, with the argument `random`, blobs at random:

```{.cpp include="examples/solutions/memory-ex3.cc" snippet="cache"}
```

It prints:

```
oldest: kept 1000024 (976 MB), dropped 999976, 23332 calls, 19 MB free
random: kept 992804 (969 MB), dropped 1007196, 63246 calls, 19 MB free
```

The cache keeps the shard full either way. The trap is about pages:
small objects that the reclaimer frees give back few whole pages. But
here, nothing needs pages. The objects that the reclaimer frees go back
to the pool of 1 KB objects, and the cache's next blobs, of the same
size, take them from there. Dropping blobs at random still works, but it
takes almost three times as many calls: the allocator asks for pages,
which random drops seldom free, so it keeps asking, while the cache's
new blobs fill the holes.

Note the comment in `main()`: the first version of this program printed
whether it dropped blobs at random by reading the lambda's capture after
a `co_await`, and printed "random" for both runs. That was
[chapter](#ch:object-lifetime)'s Lambda Coroutine Fiasco: `app.run()`
doesn't keep the lambda object alive while its coroutine waits, so after
the first `co_await`, the captures are gone. The cache keeps its own
copy of the flag.

## Exercise 4

A field `char pad[8];`, at the end of `entry`, makes the node, with its
key, 72 bytes, which the allocator serves from its pool of 80-byte
objects; 16 bytes make it 80, 24 bytes 88, served as 96, and 64 bytes
make it 128. On one shard, with random keys out of ten million, the best
of three runs, in millions of requests a second:

| Node | Allocated as | `SET` | `GET` |
|---|---|---|---|
| 64 bytes (version 12) | 64 | 3.12 | 3.40 |
| 72 bytes | 80 | 3.11 | 3.33 |
| 80 bytes | 80 | 3.10 | 3.33 |
| 88 bytes | 96 | 3.11 | 3.35 |
| 128 bytes | 128 | 3.14 | 3.35 |

The cost doesn't jump: a node of two cache lines, or one that spreads
over three, does as many `SET`s as one of a single line, and only about
2% fewer `GET`s. The
chapter's story was different because of what Shoal prefetched then: two
cache lines of each node, from its start, so the third line, where the
field that every lookup wrote was, came from main memory when the lookup
needed it. Version 12 prefetches with Seastar's `prefetch()`, which
loads every cache line of the object, however large it is, so a larger
node costs only the bandwidth of its extra lines, which arrive together
with the first. The lesson of the story is therefore narrower than "keep
nodes small": know which cache lines each lookup touches, and make sure
that all of them are prefetched, or are few.

## Exercise 5

`prefetch_ahead()` takes the distances as the indexes of the requests in
the queue of parsed requests, 7 and 3 for eight and four ahead. On one
shard, with random keys out of ten million, the best of three runs, in
millions of requests a second:

| Slot ahead, node ahead | `SET` | `GET` |
|---|---|---|
| 2, 1 | 3.30 | 3.64 |
| 4, 2 | 3.24 | 3.55 |
| 8, 4 (version 12) | 3.12 | 3.40 |
| 16, 8 | 2.97 | 3.16 |
| 32, 16 | 2.68 | 2.82 |

The shortest distances are the fastest: 2 and 1 do 7% more `GET`s than
version 12's 8 and 4. Each request takes about 300 nanoseconds, and a
load from main memory about 100, so a slot prefetched two requests ahead
has long arrived when the node is prefetched, one request ahead, and the
node has arrived when the lookup comes. A longer distance gains nothing
from that, but loses requests: counting what `prefetch_ahead()` found,
with a counter in it, showed that the request two ahead was there 96.9%
of the times, and the request eight ahead 87.5%: one in 64 fewer for
each step, so the request 32 ahead would be there only about half the
times. `valkey-benchmark` sends pipelines of 64 requests, and
for the last requests of each, fewer requests are waiting behind them,
so their slots are never prefetched. A real client's pipelines may be
shorter, which makes short distances matter more. Version 12 keeps 8 and
4, which its measurements in the chapter used; this measurement says
that 2 and 1 would be better, and why.
