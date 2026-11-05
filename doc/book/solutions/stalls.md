# Stalls: finding and preventing them {#sol:stalls}

## Exercise 1

The solution is a patch to Shoal version 12,
`examples/solutions/shoal-lazy-del.patch`. A list of more than a thousand
elements is taken out of its key's entry, before the entry is destroyed,
and destroyed by a coroutine of its own, in the background, under the
gate that `FLUSHALL ASYNC` uses, so that Shoal's stop waits for it, or
rather stops it:

```{.cpp include="examples/solutions/shoal-lazy-del.patch" file="commands.cc" snippet="lazy-free"}
```

Every command that deletes a key does it through `erase_key()`, so one
call there covers `DEL`, a key that expires, a key that `--maxmemory`
evicts, and the last `LPOP`:

```{.cpp include="examples/solutions/shoal-lazy-del.patch" file="commands.cc" snippet="erase-lazily"}
```

But two commands destroy a list without deleting its key: `SET` and
`MSET`, which can give a key that holds a list a string instead. They
call `free_lazily()` before they assign the new value. The tests check
what clients see, which passes against Valkey too:

```{.python include="examples/solutions/shoal-lazy-del.patch" file="test_solutions.py" snippet="tests"}
```

The last test is the one that matters for the gate: a `SHUTDOWN` right
after deleting a million elements must stop the server cleanly, with
status 0, whether or not the lists were destroyed by then. All 108 tests
pass. The stall hunt, repeated with the patched Shoal, found no stall:
the `DEL` of the list of five million elements returned in 3
milliseconds, and the stall detector, at 5 milliseconds, reported
nothing.

## Exercise 2

The program inserts the same million keys, in order, and shuffled:

```{.cpp include="examples/solutions/stalls-ex2.cc" snippet="orders"}
```

The best of three runs:

```
inserted in order: destroyed in 3.8 ms
inserted in a random order: destroyed in 11.1 ms
```

The table's destructor walks its list of nodes, freeing each, and after
the rehashes that the table went through as it grew, the list follows
the buckets, here the order of the keys, and not the order in which the
nodes were allocated. Inserted in order, the keys' nodes were allocated
in the same order as the buckets, one after another in memory, so the
walk goes through memory in order, which the processor's caches and
prefetching handle well. Inserted in a random order, each node in the
list is somewhere else in memory, and freeing it starts with a cache
miss: three times as long, for the same table.

## Exercise 3

`erase(begin())` is cheap for the containers made of nodes, which remove
the first node without touching the others: `std::list`, `std::map` and
`std::set`, and the unordered containers, as well as `std::deque`, which
removes from either end in constant time. For `std::vector` and
`std::string`, it moves every remaining element down, so emptying one
from the front takes time proportional to the square of its length. A
vector is emptied from the back instead, where removing an element moves
nothing:

```{.cpp include="examples/solutions/stalls-ex3.cc" snippet="vector"}
```

On ten million strings of 40 characters, each in an allocation of its
own:

```
at once: 59 ms; the heartbeat was up to 58.5 ms late
gently: 61 ms; the heartbeat was up to 0.0 ms late
```

The gentle way costs almost nothing more: the strings are freed in the
reverse order of their allocation, which suits the caches about as well.
The vector's own array, of 320 megabytes, is freed in one call at the
end, which is cheap: freeing a large allocation only returns its pages
to the shard's free lists ([chapter](#ch:memory)).

## Exercise 4

The program works for 4 milliseconds, then 5, and so on up to 14, with
the threshold at 5:

```{.cpp include="examples/solutions/stalls-ex4.cc" snippet="sweep"}
```

In three runs, the detector reported only the work of 14 milliseconds,
in two of them, as a stall of 13 milliseconds, and nothing shorter. As
[chapter](#ch:how-seastar-runs-your-code) explained, the detector checks
at intervals of the threshold plus 30%, here 6.5 milliseconds, and
reports a stall only if no task finished during a whole interval. A task
finished just before the work began, in the first interval, so the first
interval without one ends at the second check, about 13 milliseconds
after the first began. So with a threshold of 5 milliseconds, the
shortest stall that the detector reports is about 13 milliseconds; in
general, about two and a half times the threshold. A test that wants to
catch stalls of a few milliseconds needs a threshold well below them.

## Exercise 5

The pieces are sorted with `std::ranges::sort()`, and then merged, in
pairs, into a second array, yielding as it goes:

```{.cpp include="examples/solutions/stalls-ex5.cc" snippet="sort"}
```

```
at once: 631 ms, sorted: true; the heartbeat was up to 630.2 ms late
in pieces: 883 ms, sorted: true; the heartbeat was up to 1.8 ms late
```

The whole sort took 40% longer, for the merges' extra pass over the data
for each doubling, but left the shard free. The first version of this
solution sorted pieces of 100,000 numbers, and allocated the second
array as `std::vector<int>(v.size())`, and still stalled the shard for up
to 21 milliseconds: a piece of 100,000 took 5 milliseconds to sort,
which is ten task quotas, and filling the new array with zeros touched
40 megabytes of fresh memory in one step, which the chapter measured to
be slow. Pieces of 10,000, and an array that is only reserved, and
filled by the merge as it goes, removed both.
