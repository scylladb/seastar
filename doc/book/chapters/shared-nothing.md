# Shared nothing {#ch:shared-nothing}

Until now, Shoal has kept all its data on one shard, and our programs that
ran on many shards gave each shard work of its own, which never needed
anything from another. This part of the book uses all the shards together.
[Chapter](#ch:case-for-seastar) explained the idea behind Seastar's
design: data that several cores share costs locks, atomic operations and
cache lines moving between the cores, so in Seastar, each core runs one
shard, and [each shard owns its data, which no other shard
touches]{index="shared nothing" .main}. When a
shard needs something that another owns, it sends that shard a message.

This chapter makes the idea concrete. It shows what a shard owns, what
happens when code shares anyway, which C++ doesn't prevent, and how one
shard hands an object to another. [Chapter](#ch:talking-between-shards)
then shows how shards talk, and [chapter](#ch:designing-a-sharded-application)
how to divide an application's data among them.

## What a shard owns {index="shard"}

A shard is a thread, pinned to one CPU, with a part of the machine's
memory. This program asks every shard what it has, with
`smp::invoke_on_all()`, which [chapter](#ch:tutorial-introduction) used
to run a function on every shard:

```{.cpp include="examples/shared-nothing/owns.cc" snippet="owns"}
```

With four shards and 4 GB of memory (`--cpuset 0-3 -m4G`):

```
shard 0 of 4: on CPU 0, with 1024 MB of memory
shard 1 of 4: on CPU 1, with 1024 MB of memory
shard 2 of 4: on CPU 2, with 1024 MB of memory
shard 3 of 4: on CPU 3, with 1024 MB of memory
```

(in an order that changes from run to run). [`this_shard_id()`]{index="`seastar::this_shard_id()`" .main}, from
`<seastar/core/smp.hh>`, gives the calling shard's number, a `shard_id`,
and `this_smp_shard_count()` the number of shards; the shards are
numbered from 0, and the range of their numbers is
`this_smp().all_shards()`. Older code uses `smp::count`, which is
deprecated. [`memory::stats()`]{index="`seastar::memory::stats()`" .main},
from `<seastar/core/memory.hh>`, describes the calling shard's memory.

Seastar divides the memory that `-m` gives it equally among the shards,
and each shard allocates only from its own part, with an allocator of its
own, which [chapter](#ch:memory) describes. So memory that's free on one
shard doesn't help another. With 1 GB on each of four shards, shard 0
couldn't allocate a vector of 2 GB, though shard 1 had 1,005 MB free:
for a Seastar program, running out of memory means one shard running out,
and balancing the work among the shards is also balancing their memory.

Seastar divides the rest of the machine equally too. By default, each
shard accepts its share of the connections, and Seastar assumes that
every shard can do as much work as any other. On a server, whose cores
are all alike, that's true.

> **Tip: fast and slow cores.** Many laptops, and a few servers, mix fast
> cores with slower, more power-efficient ones, and a Seastar program that
> runs on both is as fast as its shards on the slow cores. On the laptop
> of [chapter](#ch:tutorial-introduction), the same computation took 0.98
> seconds on a fast core and 1.37 on a slow one. `lscpu -e` lists the
> cores and their maximum frequencies, and `--cpuset` runs the shards on
> the cores you choose; this book's measurements use the laptop's four
> fast cores, with `--cpuset 0-3`. [Chapter](#ch:deploying-and-tuning)
> returns to choosing cores.

## Sharing anyway

Nothing in C++ stops shards from sharing. All the shards are threads of
one process, so a global variable has one copy, which every shard sees.
A [`thread_local`]{index="`thread_local`"} variable, on the other hand,
has a copy in every thread, so in a Seastar program, it has one copy in
every shard. This program has both:

```{.cpp include="examples/shared-nothing/counter.cc" snippet="counters"}
```

and every shard increments one of them, a hundred million times:

```{.cpp include="examples/shared-nothing/counter.cc" snippet="increment"}
```

Both counters are `std::atomic`, so both are correct, and the shards
increment them with the same instruction. (`std::memory_order_relaxed`
asks only for the increment to be atomic, the cheapest atomic operation
there is.) The only difference is whether other shards use the same
counter. Measured on the laptop's fast cores, in three runs, with each
shard's time per increment:

| Shards | Its own counter | One shared counter |
|---|---|---|
| 1 | 6.1 ns | 6.0–6.2 ns |
| 2 | 7.2–7.3 ns | 27.0–28.5 ns |
| 4 | 7.8–7.9 ns | 63.9–66.2 ns |

With a counter of its own, each shard keeps its speed, however many
shards there are (most of the 6 nanoseconds is the loop and
`maybe_yield()`, not the increment). With a shared counter, every shard
added makes every increment slower, four shards ten times slower than
one, because of the cache-line bouncing that
[chapter](#ch:case-for-seastar) described: before a core can increment
the counter, the cache line that holds it must come from the core that
incremented it last ([Figure](#fig:counter-cost)). Four shards that share
a counter together do about as many increments a second as one shard
alone.

![The time of an increment on each shard, with one counter for each
shard (circles), and with one counter that all the shards share
(squares). With its own counter, every shard keeps its speed; with a
shared one, every shard added makes every increment
slower.](figures/counter-cost.svg){#fig:counter-cost}

A global variable that no shard changes, such as the program's
configuration, set before the shards start, is safe to share. A variable
that shards change is shared state, and it needs atomic operations, as
here, or locks, with all their costs; without them, it's a data race,
and the program's behavior is undefined. In Seastar, the answer is
neither: each shard keeps its own, as with `own_counter`, and a shard
that needs another shard's data asks it, with a message.

### Shared pointers {index="shared pointer!between shards"}

Shared pointers show the difference between sharing and not sharing in
the code that programs use most. [Chapter](#ch:object-lifetime) explained
why Seastar has pointers of its own. A `std::shared_ptr` may be copied and
destroyed on several threads at once, so it updates its count with atomic
operations. Seastar's `shared_ptr` and `lw_shared_ptr` are meant for one
shard, so they update their counts with ordinary instructions. That
chapter measured the difference with a call to a function that takes a
pointer by value, and does nothing with it: the copy increments the
pointer's count, and destroying it decrements it. On one shard, the best
of ten runs took 1.18 nanoseconds with an `lw_shared_ptr`, and 13.57 with
a `std::shared_ptr`: an atomic operation is slower than an ordinary one
even when no other core uses its cache line. Here is the same call, with
a `std::shared_ptr`, on four shards at once, each passing a pointer of
its own, or all passing copies of the same pointer, again the best of ten
runs:

| `std::shared_ptr<int>` | Time per call |
|---|---|
| on one shard | 13.57 ns |
| on four shards, each passing its own | 15.03 ns |
| on four shards, all passing the same one | 198 ns |

That's slow, but not very slow, as long as each shard passes pointers of
its own: four shards each pay about what one does. A pointer to the same
object, passed by four shards at once, cost more than ten times as
much, and more than a hundred times as much as an `lw_shared_ptr`, for
the same reason as the shared counter: the counts' cache lines move from
core to core with every call.

Seastar's pointers can't be shared that way at all, slowly or not. Here,
every shard copies a pointer that belongs to shard 0, a hundred times,
and then destroys the copies, over and over:

```{.cpp include="examples/shared-nothing/wrong-shard.cc" snippet="wrong"}
```

Each copy reads the count, adds one, and writes it back. Two shards that
do this at the same time may both read the same count, and then one
increment is lost ([Figure](#fig:count-race)); a lost decrement is just
as possible. Built against a `release` Seastar, in ten runs on four
shards, this program crashed with a segmentation fault seven times, and
aborted with `std::bad_alloc` twice; the one run that finished printed

```
4 shards: use count 5047, should be 1
```

A count that drops to zero while copies remain frees the vector they
point to, and whatever reuses its memory is then corrupted.

![Two shards that copy an `lw_shared_ptr` at the same time. Each reads
the count, adds one, and writes the result, but both read 5 before
either writes, so the count becomes 6 instead of
7.](figures/count-race.svg){#fig:count-race}

Against a `debug` Seastar, the same program stops at once, the first
time that a shard other than shard 0 touches the count:

```
ERROR ... [shard 1:main] seastar - shared_ptr accessed on non-owner cpu ...
```

In `debug` mode, every `lw_shared_ptr` and `shared_ptr` remembers the
shard that created it, and checks it whenever its count changes. That's
one more reason to run tests in `debug` mode, as
[chapter](#ch:testing) does.

So a pointer to an object that several shards use is either wrong, with
Seastar's pointers, or slow, with `std::shared_ptr`. Atomic operations
aren't forbidden in Seastar, and Seastar itself uses a few, in its queues
between shards; but code that needs them in the path of every request
has missed the point of shards. An object that several shards need is
better owned by one of them, which hands it out with a `foreign_ptr`, as
the next section shows, or copied, one copy on each shard.

### Seastar's objects belong to one shard

The pointers are only the most visible case. A shard's futures and
promises, its timers, semaphores and gates, its sockets and streams, all
belong to the shard that made them, and must be used and destroyed
there: each of them is linked to that shard's reactor, its queue of tasks
or its timers. A `file` belongs to its shard too; to open the same file on
another shard, pass that shard a `file_handle`, from
`<seastar/core/file.hh>`, which `file::dup()` makes, and which the other shard turns into a `file` of its own with
`to_file()` ([chapter](#ch:files-and-storage) shows files).

## Handing an object to another shard {index="`seastar::foreign_ptr`"}

Sending a value to another shard is easy: `smp::submit_to()` runs a
function on another shard, and gives the function's result back to the
calling shard, in a future ([chapter](#ch:talking-between-shards) shows
it in full). A result such as a number or an `sstring` is moved to the
caller, and the caller owns it from then on. (An `sstring` longer than
fourteen characters keeps its characters in memory that it allocated on
the other shard; the caller frees them, which works, but costs more than
freeing memory of its own, as [chapter](#ch:memory) explains.)

But an object that must stay on its shard can't be handed over that way:
one that has timers, for example, or is linked into the shard's data
structures, or that its shard wants back. For these, Seastar has
[`foreign_ptr`]{index="`seastar::foreign_ptr`" .main}, from
`<seastar/core/sharded.hh>`. It wraps a smart pointer, such as a
`std::unique_ptr` or an `lw_shared_ptr`, and remembers the shard that
created it, its owner. Any shard may use the object through it, and when
the `foreign_ptr` is destroyed, on whatever shard, it sends a message to
the owner, which destroys the object there. Here, shard 1 builds a table,
and gives it to shard 0:

```{.cpp include="examples/shared-nothing/foreign.cc" snippet="table"}
```

[`make_foreign()`]{index="`seastar::make_foreign()`"} wraps the pointer,
on shard 1, which therefore becomes its owner. Shard 0 uses the table,
and then destroys it:

```{.cpp include="examples/shared-nothing/foreign.cc" snippet="use"}
```

With two shards, this prints

```
shard 0 got a table that belongs to shard 1:
  row 0, made on shard 1
  row 1, made on shard 1
  row 2, made on shard 1
the table is destroyed on shard 1
back on shard 0
```

([Figure](#fig:foreign-ptr)). The table never moved: shard 0 read it in
shard 1's memory, through the pointer, and shard 1 destroyed it.
`destroy()` sends the destruction to the owner, and returns a future
that is available when it's done; the destructor of a `foreign_ptr`
sends it the same way, but without waiting. `get_owner_shard()` gives
the owner.

![`foreign.cc`'s round trip. Shard 0 asks shard 1 for a table; shard 1
builds it in its own memory, and returns a `foreign_ptr` to it. Shard 0
reads the table where it is, and `destroy()` sends it back to shard 1 to
be destroyed. Messages are solid arrows; the dashed arrow is shard 0
reading shard 1's memory directly.](figures/foreign-ptr.svg){#fig:foreign-ptr}

Doesn't shard 0's reading of shard 1's memory need the memory model that
[chapter](#ch:coordination) said Seastar avoids, with rules about which
of one core's writes another core sees, and when? It doesn't, because of
how the pointer arrived. Shard 1 wrote the table, and only then sent the
`foreign_ptr` to shard 0, in a message; and Seastar's queues of messages
between shards guarantee that whatever the sender wrote before it sent a
message is visible to the receiver when the message arrives. What a
`foreign_ptr` doesn't do is make the object safe to change while other
shards use it: shard 1 handed the table over, and mustn't touch it again
until it gets it back, to destroy it. A shard that keeps changing an
object should answer questions about it, with messages, instead of
handing it out.

A `foreign_ptr` can't be copied, only moved, because a copy would be a
second owner. To give another shard a second pointer to a shared object,
[`copy()`]{index="`seastar::foreign_ptr`!`copy()`"} copies the wrapped
pointer, an `lw_shared_ptr` or a `shared_ptr`, on the owner shard, where
its count may change, and returns a future of a new `foreign_ptr`. And
`release()` takes the wrapped pointer out of the `foreign_ptr`, leaving
the caller responsible for destroying it on the owner shard, so it's best
called there; `reset()` replaces it, destroying the old one on its owner.

## Exercises

1. Change `foreign.cc` so that shard 1 returns just the rows, a
   `std::vector<sstring>`, by value, instead of a `foreign_ptr` to a
   table. Which shard frees the strings now? Compare
   `memory::stats().cross_cpu_frees()` on shard 0 before and after it
   destroys the vector: how many frees crossed shards, and why that
   many? Why does the program still work? What would go wrong if shard 1
   returned something that held an `lw_shared_ptr`, or a timer?

2. Make shard 1 build the table in an `lw_shared_ptr`, and give shard 0 a
   `foreign_ptr` to it. Shard 0 then makes a second `foreign_ptr` with
   `copy()`, and gives it to shard 2, and each of the two shards destroys
   its own pointer. On which shard does `copy()` run, and why must it?
   When, and where, is the table destroyed?

3. Build `wrong-shard.cc` against a `debug` Seastar, and run it. Which
   shard reports the error? Then write a program in which only shard 0
   copies an `lw_shared_ptr`, into the capture of a lambda given to
   `smp::submit_to(1, ...)`, and shard 1 only reads the vector through
   it. Is that a bug too? Does a `debug` build object? (`submit_to()`'s
   documentation says where the lambda is destroyed.) And if the lambda,
   on shard 1, copies the pointer once more?

4. Run `owns.cc` with `-c4 -m4G`, and then make shard 0 allocate a vector
   of 512 MB, of 900 MB and of 2 GB. Which allocations succeed? How much
   does `memory::stats()` say is free on each shard afterwards? What does
   this mean for a program whose data isn't divided evenly among its
   shards? ([Chapter](#ch:memory) explains the one result that may
   surprise you.)
