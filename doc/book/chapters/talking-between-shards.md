# Talking between shards {#ch:talking-between-shards}

[Chapter](#ch:shared-nothing) showed that shards shouldn't share: each
shard owns its memory and its objects, and an object that another shard
touches, even only to copy a pointer to it, is slow, or broken. Shards
cooperate instead by sending each other messages: a shard asks another
to run a function, and gets the result back. This chapter is about those
messages: how to send them, how they travel, what they cost, and how to
organize a program around them, with a service that has an instance on
every shard. And it's about the way messages can go wrong: calls that
wait for each other, forever.

[Chapter](#ch:designing-a-sharded-application) then uses all this to
make Shoal use every core.

## Sending work to another shard {index="message passing"}

[`smp::submit_to()`]{index="`seastar::smp::submit_to()`" .main}, from
`<seastar/core/smp.hh>`, runs a function on another shard, and returns a
future, on the calling shard, for the function's result. The function
may return a value, or a future, in which case the caller's future is
ready when that one is. A coroutine that asks shard 1 for something
looks like a local call with a `co_await`:

```cpp
auto n = co_await smp::submit_to(1, [] { return compute_something(); });
```

What travels between the shards is the function object, with its
captures, and the result. [Chapter](#ch:shared-nothing) gave the rules
for what may travel. A result, such as a number or an `sstring`, is
moved to the calling shard, which owns it from then on. A capture must
be safe to use on the other shard: a copy of a value is, and so is a
reference to an object that the calling shard keeps alive, and doesn't
change, until the result comes back, as long as the other shard only
reads it; an object that belongs to its shard, such as a timer or an
`lw_shared_ptr`, isn't. The function object itself is moved and
destroyed on the calling shard, so its captures are freed where they
were allocated.

When the shard is the caller's own, `submit_to()` makes no message: it
calls the function directly. Two relatives send work to many shards:
[`smp::invoke_on_all()`]{index="`seastar::smp::invoke_on_all()`" .main},
which [chapter](#ch:tutorial-introduction) used, runs a function on
every shard, the calling one too, and
[`smp::invoke_on_others()`]{index="`seastar::smp::invoke_on_others()`"}
on every shard but the calling one. Both return a future that is ready
when every shard is done.

### How a message travels

Every pair of shards has a pair of queues, one for each direction: shard
0's requests to shard 1 go through one, and shard 1's replies back
through the other. Each queue is a fixed-size ring buffer, which one
shard writes and the other reads, without a lock. A request goes through
these steps ([Figure](#fig:message-path)):

![How `submit_to()` sends a request from shard 0 to shard 1, and gets
its reply. Each direction has its own queue. Requests and replies wait
in batches, and each shard picks up what was sent to it when it
polls.](figures/message-path.svg){#fig:message-path}

1. `submit_to()` takes a unit of a semaphore that limits how many of the
   calling shard's requests may be in progress on the other shard, and
   puts the request in a list of pending requests. (By default, the
   semaphore has no limit; "Calls that wait for each other", below,
   shows the other kind.)
2. When 16 requests are pending, or when the reactor next polls, the
   list is pushed into the queue, as a batch. If the other shard is
   asleep, because it had no work, it's woken up.
3. The other shard finds the requests when it polls, and runs each one's
   function, as a task.
4. When a function's result is ready, the reply waits in the same way,
   for a batch of 16 or the next poll, and goes back through the queue
   of replies.
5. The calling shard finds the reply when it polls, makes the future
   ready, and gives the semaphore's unit back.

A message, then, is cheap to send: a few writes to memory, and no
system call. What it costs is mostly waiting: for a batch to fill, or
for the other shard to poll, and for the cache lines that hold the
request to move from one core to the other, as
[chapter](#ch:shared-nothing)'s shared counter showed they must.

### What a hop costs {index="message passing!cost"}

A message's round trip, from one shard to another and back, is called a
*hop*. This program measures hops, with requests that do nothing, so
that the time is the hop's own:

```{.cpp include="examples/talking-between-shards/hop.cc" snippet="round-trips"}
```

It sends a million of them, from shard 0 to the shard given with `--to`,
and with `--in-flight`, it runs several fibers that send them at the
same time, each one waiting for its own replies:

```{.cpp include="examples/talking-between-shards/hop.cc" snippet="in-flight"}
```

On two of the laptop's fast cores (`--cpuset 0-1`), the best of ten
runs:

| From shard 0 | Time per request | Requests a second |
|---|---|---|
| to shard 0 (no message) | 2 ns | 591 million |
| to shard 1, one at a time | 950 ns | 1.05 million |
| to shard 1, 10 at a time | 129 ns | 7.72 million |
| to shard 1, 100 at a time | 45 ns | 22.0 million |
| to shard 1, 1,000 at a time | 46 ns | 21.7 million |
| to shard 1, one at a time, `--idle-poll-time-us 0` | 5,355 ns | 0.19 million |

A request to another shard, one at a time, takes almost a microsecond,
about 500 times as long as calling the function directly. A request
waits twice for a poll, once on each side, and since nothing else is
pending, each batch holds a single request. With many requests in
flight, the hops overlap: batches fill up, each poll finds many requests,
and the cost of each falls twenty-fold, to 45 nanoseconds, where it stays
once the batches are full. The time of a single request is still a
microsecond or more; it's the throughput that grows.

The last row shows what idle polling, from
[chapter](#ch:how-seastar-runs-your-code), is for. By default, a shard
that runs out of work polls for 200 microseconds before it goes to
sleep, and a message that arrives in that time is found at once. With
`--idle-poll-time-us 0`, shard 1 sleeps between requests, and every
request must wake it up through the kernel: the hop takes five times as
long. That's the price of `--overprovisioned`, which turns idle polling
off, on a machine whose cores the shards share with other programs.

So a hop costs far more than a function call, and a design must account
for it: send work to another shard in batches, or keep many requests in
flight, and don't send tiny pieces of work one at a time, in a loop
that waits for each. The rest of this chapter shows both, with a service
that lives on every shard.

## Sharded services {index="sharded service"}

A *service*, in Seastar's vocabulary, is an object that does some job
for the rest of the program: a cache, a server, a store of data. A
[*sharded service*]{index="sharded service" .main} has an instance on every shard, each with
its own share of the job: its own part of the cache, the connections
that its shard accepted, the data that belongs to its shard. Seastar's
[`sharded<T>`]{index="`seastar::sharded`" .main}, from
`<seastar/core/sharded.hh>`, holds the instances of a service of class
`T`: it constructs one on every shard, lets code reach any of them, and
stops and destroys them all at the end. It's the backbone of every large
Seastar program: ScyllaDB has about 1,600 uses of `sharded`, and Redpanda
2,900.

This program counts the words of a made-up text, a million of them, of
100,000 different words, with a service whose instance on each shard
counts the words that belong to that shard. Which shard a word belongs
to is decided by its hash:

```{.cpp include="examples/talking-between-shards/wordcount.cc" snippet="service"}
```

Spreading data over the shards by a hash of its key is the most common
way to divide work in a Seastar program: every shard owns a part of the
keys, and every key has exactly one owner, which any shard can compute.
[Chapter](#ch:designing-a-sharded-application) does the same with
Shoal's keys.

### Starting and stopping

A `sharded<word_counter>` is first empty.
[`start()`]{index="`seastar::sharded`!`start()`"} constructs an instance
on every shard, passing a copy of its arguments, if any, to each
constructor, and returns a future that is ready when all of them have
been constructed.
[`stop()`]{index="`seastar::sharded`!`stop()`"} calls each instance's
`stop()`, if the class has one, which returns a future, waits for all
of them, and then destroys the instances, each on its own shard. Here
`word_counter` has no `stop()`: it has nothing to wait for.

`stop()` must be called before the `sharded` object is destroyed, and
it can't be called from a destructor, which can't wait, so the code that
starts a service must make sure to stop it, however its work ends:

```{.cpp include="examples/talking-between-shards/wordcount.cc" snippet="lifecycle"}
```

That's the pattern of [chapter](#ch:concurrency-within-a-shard)'s
`handle_connection()`: remember a failure, clean up, and then pass the
failure on, with `coroutine::return_exception_ptr()`. Destroying a
`sharded` object that is still started isn't left to chance:
`sharded`'s destructor checks it, and stops the program if it is.

Between the two, two functions reach the instances.
[`local()`]{index="`seastar::sharded`!`local()`"} returns a reference to
the calling shard's instance. And
[`invoke_on()`]{index="`seastar::sharded`!`invoke_on()`"} is
`submit_to()` for a service: it runs a function, given the instance on
the shard that it names, there, and returns a future for the function's
result. Its relatives
[`invoke_on_all()`]{index="`seastar::sharded`!`invoke_on_all()`"} and
`invoke_on_others()` run a function with every instance, or every other
one; ScyllaDB and Redpanda call `invoke_on_all()` about a thousand times.
The program's last step collects the counts of all the instances, with
[`map_reduce0()`]{index="`seastar::sharded`!`map_reduce0()`"}: it runs
its first function on every shard, with the instance there, and combines
the results, as they arrive, on the calling shard, with the second,
starting from the initial value it's given: here, `std::plus`, with
`counts`'s `operator+`. [`map()`]{index="`seastar::sharded`!`map()`"}
returns all the results instead, in a vector, in the order of the shards.

### Talking to a peer

An instance often needs to reach the others. A class that derives from
[`peering_sharded_service<T>`]{index="`seastar::peering_sharded_service`"}
gets a method,
[`container()`]{index="`seastar::sharded`!`container()`"}, that returns
the `sharded<T>` that holds it, and through it, any of its peers. That's
how `word_counter::count()` sends a word to the shard it belongs to,
wherever it's called. The first way to count the words calls it for one
word at a time:

```{.cpp include="examples/talking-between-shards/wordcount.cc" snippet="one"}
```

The second collects the words for each shard, ten thousand words at a
time, and sends each shard its batch, all shards at once:

```{.cpp include="examples/talking-between-shards/wordcount.cc" snippet="batched"}
```

The function captures its batch by reference, and the shard that runs it
reads the words from shard 0's memory. That follows
[chapter](#ch:shared-nothing)'s rules: shard 0 keeps the batches alive,
and doesn't change them, until all the replies have come, and the other
shard only reads them, copying each word it counts into its own memory.

The best of ten runs, on one, two and four of the laptop's fast cores:

| Shards | One word at a time | In batches |
|---|---|---|
| 1 | 89 ns per word | 68 ns per word |
| 2 | 545 ns per word | 79 ns per word |
| 4 | 1,108 ns per word | 75 ns per word |

With one shard, every word is local, and `invoke_on()` calls the function
directly. With two, half of the words take a hop, of about a
microsecond, and with four, three quarters of them do; one word at a
time is twelve times slower than with one shard. In batches, the hops
cost almost nothing: a batch of several thousand words takes one hop.

But four shards in batches are no faster than one shard. Every word is
made, hashed and put in its batch by shard 0, and that costs about as
much as counting it. The other shards spend most of their time waiting
for shard 0, which does most of the work alone, and a program can't be
faster than its busiest shard. Dividing work over shards pays only when
the work is divided, not only the data it ends up in: here, the making of
the words must be divided too, as exercise 2 does.
[Chapter](#ch:designing-a-sharded-application) meets the same problem
in Shoal, where every shard must accept connections, not only own keys.

## Starting a service in other ways

Not every instance should be the same. A function wrapped in
[`sharded_parameter`]{index="`seastar::sharded_parameter`"} is called on
each shard, when `start()` constructs that shard's instance, and its
result is passed to that instance's constructor:

```{.cpp include="examples/talking-between-shards/startup.cc" snippet="parameter"}
```

```
hello from shard 0
hello from shard 1
hello from shard 2
```

Typical uses give each instance its own copy of a configuration, or its
own share of a resource, or a reference to the same shard's instance of
another sharded service, which the function returns as
`std::ref(other.local())`. Redpanda uses `sharded_parameter` about 300
times, for all of these.

Some services need only one instance: a coordinator, a cache of
something small, a client of a slow external service.
[`start_single()`]{index="`seastar::sharded`!`start_single()`"} starts
just one instance, on shard 0, and the other shards reach it with
`invoke_on(0, ...)`:

```{.cpp include="examples/talking-between-shards/startup.cc" snippet="single"}
```

```
shard 1 has an instance: false
shard 1 asks shard 0: hello from the only instance
shard 1 asks shard 1: sharded instance does not exist: greeter
```

[`local_is_initialized()`]{index="`seastar::sharded`!`local_is_initialized()`"}
tells whether the calling shard has an instance. Calling `invoke_on()`
for a shard that has none fails, with
[`no_sharded_instance_exception`]{index="`seastar::no_sharded_instance_exception`"}.
Calling `local()` there is worse: it stops the program, since it returns
a reference, and has nothing to refer to. Redpanda starts about 40
services with `start_single()`. ScyllaDB doesn't use it at all. Its
services have an instance on every shard, even where only shard 0's
does some of the work, and the others forward that work to it, with
`invoke_on(0, ...)`, which ScyllaDB calls more than a hundred times. It's
the same "only on shard 0" design, hidden behind the service's
interface, so that any shard can call `local()`.

One more base class changes how a service stops. An instance may be in
use, from other code on its shard, when `stop()` is called: a background
fiber that holds a pointer to it, for example. A class that derives from
[`async_sharded_service<T>`]{index="`seastar::async_sharded_service`"}
can hand out pointers to itself with `shared_from_this()`, as
[chapter](#ch:object-lifetime)'s sessions did, and `sharded::stop()`
then waits until every such pointer, on every shard, has been released,
before it returns.

### Values without a lifecycle {index="`thread_local`!as a sharded value"}

Not everything per shard needs a `sharded<T>`. A
`thread_local` variable, as [chapter](#ch:shared-nothing) showed, has a
copy on every shard, and needs no starting or stopping: it's constructed
before its first use on each shard, and destroyed when the shard's
thread ends. That's enough for statistics, counters, or a cache whose
entries can simply be dropped, and Seastar programs use `thread_local`
for such things everywhere. But nothing can wait for a `thread_local`
object's cleanup: by the time it's destroyed, its shard's reactor has
stopped, and the destructor can't wait for a future, or send a message.
Anything with work in progress, with connections, files, timers or
background fibers, belongs in a service with a `stop()`, in a
`sharded<T>`.

## Calls that wait for each other {index="deadlock!between shards"}

A shard that receives requests from every other shard can be swamped.
Each sender may limit its own requests, with a semaphore, as
[chapter](#ch:coordination) showed, but on a machine of 64 cores, the
receiver gets 63 times what any sender allows. An
[`smp_service_group`]{index="`seastar::smp_service_group`" .main}, from
`<seastar/core/smp.hh>`, limits the requests that a group of calls may
have in progress on each shard. It's created with
[`create_smp_service_group()`]{index="`seastar::create_smp_service_group()`"},
given an `smp_service_group_config` whose `max_nonlocal_requests` says
how many requests of the group a shard may be running at once, from all
the other shards together; each sender gets an equal share, and since
each needs at least one, a limit smaller than the number of other shards
is quietly raised to it. A call joins
a group by passing it to `submit_to()`, or in an `smp_submit_to_options`,
which can also set a timeout for the wait for the group's unit, the
first step in [Figure](#fig:message-path). A group that's no longer
needed is destroyed with `destroy_smp_service_group()`.

The default group, which every call without one uses, has no limit.
Both large projects create groups of their own: Redpanda's code names
`smp_service_group` about 190 times, and ScyllaDB's about 30.

A group's limit comes with a danger, which Seastar's documentation
names: calls in a group must not *nest*. This program makes two shards
call each other at the same moment, and each call, on the other shard,
makes a nested call back:

```{.cpp include="examples/talking-between-shards/nested.cc" snippet="nested"}
```

It runs this twice, first with one group for both levels of calls, and
then with a group for each level, both groups allowing one call at a
time between two shards:

```{.cpp include="examples/talking-between-shards/nested.cc" snippet="groups"}
```

With two shards (`-c2`), it printed, a second later:

```
one group: seastar::named_semaphore_timed_out (Semaphore timed out: smp_service_group#1 1->0 semaphore)
two groups: done
```

With one group, the nested calls never ran: they waited for their turn
until the timeout ended the wait. Shard 0's call to shard 1 holds the one
unit for calls from shard 0 to 1, and it keeps it until its reply comes
back; meanwhile, on shard 1, it makes a nested call to shard 0, which
needs the one unit for calls from shard 1 to 0. That unit is held by
shard 1's own call, which, on shard 0, makes a nested call that needs
the unit that shard 0's call holds ([Figure](#fig:nested-deadlock)).
Each call waits for the other, and without the timeout, they would wait
forever. (Without the 10 milliseconds of work before the nested call,
the program didn't deadlock: each nested call ran before the other
shard's call had taken its unit. Such deadlocks depend on timing, which
makes them rare in tests, and real in production.)

![The deadlock of `nested.cc`, with one group for both levels of calls.
Each shard's call holds the one unit for calls from its shard to the
other, and its nested call waits for the unit that the other call
holds.](figures/nested-deadlock.svg){#fig:nested-deadlock}

With a group for each level, the nested calls take units of the second
group, which nobody holds, and both finish. That's the rule that
Seastar's documentation gives: the groups of nested calls must form a
directed acyclic graph. A call in one group may make calls in another,
but never, directly or through further calls, in its own. The default
group can't deadlock this way, since it never makes a call wait, which
is why it's the default: a program can make its cross-shard calls
correctly first, and limit the heavy ones later, each in a group of its
own.

## Exercises

1. Run `hop.cc` with 1, 2, 4, 8, 16, 32 and 64 requests in flight. Up to
   how many does doubling them halve the time per request, and why? Why
   does it stop halving, and what is left of a request's cost when many
   are in flight?

2. In `wordcount.cc`'s batched counting, every word is made by shard 0.
   Make every shard make its share of the words instead, the words whose
   numbers it gets when the numbers are dealt out round-robin, and send
   its batches to their shards, as shard 0 did. How fast is counting
   now, with one, two and four shards?

3. Remove `counters.stop()` from `wordcount.cc`. What happens when the
   program ends? Then put the call back, and give `word_counter` a
   `stop()` that prints its shard's number. In which order do the shards
   stop?

4. In `nested.cc`, give each group two units instead of one
   (`max_nonlocal_requests = 2`). Does the first case still deadlock?
   Then make each shard start two calls at once, instead of one. Does it
   deadlock now? What does that tell you about making a deadlock less
   likely, rather than impossible?
