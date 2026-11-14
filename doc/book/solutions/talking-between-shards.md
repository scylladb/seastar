# Talking between shards {#sol:talking-between-shards}

## Exercise 1

`hop.cc`, from shard 0 to shard 1 on two of the laptop's fast cores
(`--cpuset 0-1 --to 1`), the best of five runs, with the chapter's
numbers for 1 and 100:

| In flight | Time per request |
|---|---|
| 1 | 950 ns |
| 2 | 491 ns |
| 4 | 263 ns |
| 8 | 161 ns |
| 16 | 95 ns |
| 32 | 61 ns |
| 64 | 49 ns |
| 100 | 45 ns |

Up to four requests in flight, doubling them nearly halves the time per
request. A round trip takes about a microsecond, almost all of it
waiting, for the other shard to poll and for the reply to come back, and
requests in flight together wait together: two requests share a round
trip, and each costs half of it, four share it four ways.

Then the halving slows down, and the time approaches 45 nanoseconds,
which more requests no longer reduce. That's what a request costs in
work, not in waiting: shard 0 makes the request, with its function and
its promise, takes its semaphore unit and puts it in the queue; shard 1
takes it out, runs it as a task, and sends the reply; shard 0 makes its
future ready, and gives the unit back. With many requests in flight, the
shards never wait, and this work is all that's left. So keeping many
requests in flight hides the round trip's waiting, but not the work of
each request: to go below 45 nanoseconds per piece of work, put many
pieces in one request, as `wordcount.cc`'s batches do.

## Exercise 2

The solution makes counting a method of the service, which
every shard runs on its share of the words, and sends its batches to
their shards through `container()`:

```{.cpp include="examples/solutions/talking-between-shards-ex2.cc"}
```

The best of five runs, against the chapter's batched counting, in which
shard 0 makes every word:

| Shards | Shard 0 makes the words | Every shard makes its share |
|---|---|---|
| 1 | 68 ns per word | 73 ns per word |
| 2 | 79 ns per word | 41 ns per word |
| 4 | 75 ns per word | 25 ns per word |

Now more shards count faster: with four, almost three times as fast as
with one. Each shard makes a quarter of the words, hashes them, and
sends them in batches to their shards, while the others do the same, so
the work that held shard 0 back in the chapter is divided too. Not quite
four times as fast: the shards now also do things that one shard didn't
need, sending batches to every other shard, and reading words that
another shard wrote. With one shard, the new version is a little slower
than the old one, since it does the same work through a few more steps.

That's the lesson that [chapter](#ch:designing-a-sharded-application)
starts from: dividing the data among the shards isn't enough, the work
that produces it, in a server the connections that bring the requests,
must be divided too.

## Exercise 3

Without `counters.stop()`, the program stopped at the end, with exit
status 134:

```
.../seastar/core/sharded.hh:592: seastar::sharded<Service>::~sharded() [with Service = word_counter]: Assertion `_instances.empty()` failed.
Aborting on shard 0, in scheduling group main. Backtrace: ...
```

`counters` is a local variable of the main function, and its destructor
found the instances still there. A destructor can't wait, so it can't
stop them, and a `sharded` object destroyed with its instances would
leave every shard with an instance that nothing can reach or destroy,
whose work may still be running. So the destructor checks, in every
build, and stops the program at the mistake. (The line with the counts
didn't appear: it was still in the buffer of the standard output, which
an abort doesn't flush.)

With the call back, and a `stop()` that says which shard stops:

```{.cpp include="examples/solutions/talking-between-shards-ex3.cc"}
```

five runs with four shards printed:

```
shard 0 stops, shard 1 stops, shard 2 stops, shard 3 stops
shard 0 stops, shard 3 stops, shard 1 stops, shard 2 stops
shard 0 stops, shard 1 stops, shard 3 stops, shard 2 stops
shard 0 stops, shard 3 stops, shard 2 stops, shard 1 stops
shard 0 stops, shard 2 stops, shard 1 stops, shard 3 stops
```

(each on a line of its own). The shards stop at the same time, not one
after the other: `sharded::stop()` sends every shard its call of
`stop()` at once, with `parallel_for_each()`, and destroys the instances
only after all of them have stopped. Shard 0, the calling shard, is
always first, because a call to the calling shard needs no message, and
runs at once; the others' order depends on which shard gets to its
message first. So a service's `stop()` must not assume that another
shard's instance is still running, or already stopped: in the middle of
a stop, both are possible.

## Exercise 4

A program that takes the group's units and the number of calls each
shard starts at once as options:

```{.cpp include="examples/solutions/talking-between-shards-ex4.cc"}
```

In three runs of each (`-c2`):

| Units | Calls at once on each shard | Result |
|---|---|---|
| 1 | 1 | deadlock (the timeout) |
| 2 | 1 | done |
| 2 | 2 | deadlock |
| 3 | 2 | done |
| 4 | 2 | done |
| 4 | 4 | deadlock |
| 4 | 5 | deadlock |

With two units, one call at a time no longer deadlocks: each outer call
holds one of its shard's two units, and its nested call takes the other.
But two calls at once deadlock again, and so does every case where each
shard starts as many calls as the group has units, or more: the outer
calls hold every unit, and every nested call waits for one of them.

So a larger limit doesn't remove the deadlock; it only raises the load
at which it happens. In a test, with little load, it may never happen;
in production, the first burst of requests that fills the group locks
up both shards. And a limit is there precisely to be reached, under
load. The only cure is the chapter's rule: nested calls in groups of
their own, so that the calls that hold units never wait for units of the
same group.
