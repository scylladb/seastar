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
`<seastar/core/smp.hh>`, runs a function on another shard, the
*receiving* shard, and returns a future, on the calling shard, for the
function's result. The function
may return a value, or a future, in which case the caller's future is
ready when that one is. A coroutine that asks shard 1 to count how many
times `word`, an `sstring`, appears in `text`, a vector of them, looks
like a local call with a `co_await`:

```cpp
auto n = co_await smp::submit_to(1, [&text, word] {
    return std::ranges::count(text, word);
});
```

What travels between the shards is the function object, with its
captures, and the result. A result, such as a number or an `sstring`, is
moved to the calling shard, which owns it from then on. The function
object is made, moved and destroyed on the calling shard, so its
captures are copied and freed there; only the call runs on the
receiving shard ([Figure](#fig:submit-lifetime)). What matters is what the
function does with its captures there:

![Where the parts of the `submit_to()` call above live and run. The
lambda, with its copy of `word`, is made in shard 0's memory, and stays
there: shard 1 calls it where it is, and reads `text` and the copy of
`word` in shard 0's memory. Shard 0 destroys the lambda when the reply
comes back. Messages are solid arrows; the dashed arrow is shard 1
reading shard 0's memory directly.](figures/submit-lifetime.svg){#fig:submit-lifetime}

- A capture that is a copy of a value, such as `word`, belongs to the
  function, which may use it as it likes.
- A capture that refers to an object of the calling shard, such as the
  reference to `text`, is safe as long as the receiving shard only reads
  the object, and the calling shard keeps it alive, and doesn't change
  it, until the result comes back. Here, the coroutine waits for the
  result with `co_await`, but that isn't enough: `text` must also
  outlive the wait, and no other fiber on shard 0 may change it
  meanwhile.
- Beware of operations that seem to only read, but write. A captured
  `lw_shared_ptr` is the usual case. Following the pointer, with `*` or
  `->`, only reads it, and is fine. But copying it by accident, for
  example by passing it by value to another function, increments its
  reference count: a write, on a shard that may not make it.
  [Chapter](#ch:shared-nothing) showed what happens when two shards
  change one count: updates are lost, and the object is freed while
  copies of it remain.
- An object that is linked into its shard's reactor, such as a timer or
  a semaphore, can't be used on another shard at all: arming the timer,
  or waiting on the semaphore, changes the lists of tasks or timers that
  one shard keeps.

The calling shard could promise not to touch the pointer while the
receiving shard works, but that's rarely a promise it can keep: an
object is put in an `lw_shared_ptr` because several fibers use it at
once, and they copy the pointer and drop their copies whenever they
like. A shared object should travel in a `foreign_ptr` instead. Here,
shard 0 keeps a dictionary in an `lw_shared_ptr`, and asks shard 1 to
count its long words:

```{.cpp include="examples/talking-between-shards/dictionary.cc" snippet="send"}
```

`make_foreign()` copies the `lw_shared_ptr`, on shard 0, and wraps the
copy in a `foreign_ptr`, which the lambda captures. Shard 1 reads the
dictionary through it, and passes the dictionary on by reference. A copy
of `d` would be a compile error, not a crash, because a `foreign_ptr`
can't be copied; a receiving shard that needs a copy asks for one with
`copy()`, which sends a message to the owner, and makes the copy there.
The lambda, and with it the `foreign_ptr`, is destroyed on shard 0, the
`foreign_ptr`'s owner, which drops its copy of the `lw_shared_ptr` right
there, without a message. So the count changes only on shard 0, however
many fibers there use the dictionary meanwhile. The program prints

```
shard 1 counted 2 long words in shard 0's dictionary
the dictionary is destroyed on shard 0
```

A `foreign_ptr` doesn't make the dictionary safe to change, though:
it's `const`, and nothing changes it while shard 1 reads it. And if
shard 1 must keep the dictionary after the call, it moves the
`foreign_ptr` out of the lambda, which must then be `mutable`; when
shard 1 destroys it, the `foreign_ptr` sends its copy back to shard 0,
to be dropped there.

When the shard is the caller's own, `submit_to()` makes no message: it
calls the function directly.

Seastar also provides two functions to send work to many shards:
[`smp::invoke_on_all()`]{index="`seastar::smp::invoke_on_all()`" .main},
which [chapter](#ch:tutorial-introduction) used, runs a function on
every shard, the calling one too, and
[`smp::invoke_on_others()`]{index="`seastar::smp::invoke_on_others()`"}
on every shard but the calling one. Both return a future that is ready
when every shard is done.

### How a message travels

A request is an object that `submit_to()` allocates in the calling
shard's memory ([Figure](#fig:submit-lifetime)). It holds three things:
the function, with its captures; room for the function's result; and
the promise of the future that `submit_to()` returned to the caller. The
request itself never moves: the queues between the shards carry only a
pointer to it, to the receiving shard, and then back, as the reply.

A shard that sends requests to another has two queues for them: one for
its requests, and one for their replies. Shard 0's requests to shard 1
go through one, and shard 1's replies to them come back through the
other. Shard 1's own requests to shard 0 have a pair of queues of their
own, so two shards are joined by four queues, and a reply never shares
a queue with a request. Each queue is a fixed-size ring buffer, which
its sender writes and its receiver reads, without a lock. A request goes
through these steps ([Figure](#fig:message-path)):

![How `submit_to()` sends a request from shard 0 to shard 1, and gets
its reply. The requests and the replies have queues of their own.
Requests and replies wait
in batches, and each shard picks up what was sent to it when it
polls.](figures/message-path.svg){#fig:message-path}

1. `submit_to()` takes a unit of a semaphore that limits how many of the
   calling shard's requests may be in progress on the receiving shard, and
   puts the request in a list of pending requests. (By default, the
   semaphore has no limit; "Limiting the calls a shard receives", below,
   shows the other kind.)
2. When 16 requests are pending, or when the reactor next polls, the
   list is pushed into the queue, as a batch. If the receiving shard
   is asleep, because it had no work, it's woken up.
3. The receiving shard finds the requests when it polls, and runs each
   one's function, as a task. It calls the function where it is, in the
   request, in the calling shard's memory.
4. When a function's result is ready, the receiving shard stores it in
   the request, and the request itself becomes the reply: the same
   pointer goes back, through the queue of replies, after waiting in the
   same way, for a batch of 16 or the next poll. Replies needn't come
   back in the order of their requests. A function that returns a future
   replies only when that future is ready, so a slow request may reply
   after quicker ones that were sent after it.
5. The calling shard finds the reply when it polls. It needs no matching
   to know which call the reply answers: the reply is the request, and
   the request holds its caller's promise. The calling shard moves the
   result into that promise, which makes the caller's future ready, then
   destroys the request, with the function and its captures, and gives
   the semaphore's unit back.

A message, then, is cheap to send: a few writes to memory, and no
system call. What it costs is mostly waiting: for a batch to fill, or
for the receiving shard to poll, and for the cache lines that hold the
request to move from the calling shard's core to the receiving shard's, as
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
once the batches are full. The latency of a single request is still a
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
Seastar program: ScyllaDB has about 80 different sharded services, and
Redpanda about 160.

A `sharded<T>` is one object, which the program creates on one shard,
usually shard 0. It isn't a container of copies of one `T`. Inside, it
has a table with an entry for every shard, which points to that shard's
instance of `T`. Each instance is constructed on its own shard, so its
memory comes from that shard, and it holds that shard's data, not a copy
of another instance's. The table doesn't change while the service runs,
so every shard can use the same `sharded<T>` object, through a reference
to it. Its method `local()` returns the calling shard's own instance,
and `invoke_on()` runs a function on another shard, with the instance
there. The rest of this section shows these methods, with an example.

The following program counts the words of a made-up text, a million of them, of
100,000 different words, with a service whose instance on each shard
counts the words that belong to that shard. Which shard a word belongs
to is decided by its hash:

```{.cpp include="examples/talking-between-shards/wordcount.cc" snippet="service"}
```

`word_counter` is an ordinary class, and the next section shows how the
program keeps its instances in a `sharded<word_counter>`. The base
class, `peering_sharded_service`, doesn't replace `sharded`, despite its
name. It only gives each instance a method, `container()`, through
which it reaches the instances on the other shards, as `count()` does,
and which this section explains below.

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
failure on, with `coroutine::return_exception_ptr()`. A forgotten `stop()` doesn't go
unnoticed: if a `sharded` object is destroyed after `start()` but before
`stop()`, while its instances still exist, its destructor aborts the
program.

Another base class, besides `peering_sharded_service`, changes how a
service stops. An instance may be in
use, from other code on its shard, when `stop()` is called: a background
fiber that holds a pointer to it, for example. A class that derives from
[`async_sharded_service<T>`]{index="`seastar::async_sharded_service`"}
can hand out pointers to itself with `shared_from_this()`, as
[chapter](#ch:object-lifetime)'s sessions did, and `sharded::stop()`
then waits until every such pointer, on every shard, has been released,
before it returns.

### Reaching the instances

Between `start()` and `stop()`, code reaches the instances through the
`sharded` object.
[`local()`]{index="`seastar::sharded`!`local()`"} returns a reference to
the calling shard's instance. The program's first way to count the words
uses it to call its own shard's `count()`, for one word at a time:

```{.cpp include="examples/talking-between-shards/wordcount.cc" snippet="one"}
```

`count()` sends the word to the shard that it belongs to. For that, it
needs the `sharded<word_counter>`, and it gets it from
[`container()`]{index="`seastar::sharded`!`container()`"}, a method that
`word_counter` inherits from
[`peering_sharded_service<T>`]{index="`seastar::peering_sharded_service`"}.
It returns the `sharded<T>` that holds the instance, through which an
instance reaches any of its peers. Then
[`invoke_on()`]{index="`seastar::sharded`!`invoke_on()`"} is
`submit_to()` for a service: it runs a function on the shard that it
names, gives the function the instance there, and returns a future for
the function's result.

The second way collects the words for each shard, ten thousand words at
a time, and sends each shard its batch, all shards at once, with
`invoke_on()`:

```{.cpp include="examples/talking-between-shards/wordcount.cc" snippet="batched"}
```

The lambda passed to `invoke_on()` captures its batch by reference, and
the shard that runs it reads the words from shard 0's memory. That follows
[chapter](#ch:shared-nothing)'s rules: shard 0 keeps the batches alive,
and doesn't change them, until all the replies have come, and the
receiving shards only read them, copying each word they count into
their own memory.

Where `invoke_on()` runs a function with one instance, `sharded<T>` has
more methods that run a function with several instances at once:

- [`invoke_on_all()`]{index="`seastar::sharded`!`invoke_on_all()`"}
  runs it with every instance, and `invoke_on_others()` with every
  instance but the calling shard's. Like the batched loop above, they
  send it to all the shards at once, and return a future that is ready
  when all have finished. But they send every shard the same function,
  a copy of it for each, while the loop sends each shard its own batch,
  so it calls `invoke_on()` for each shard itself.
- [`map_reduce0()`]{index="`seastar::sharded`!`map_reduce0()`"} runs it
  with every instance, and combines the results, as they arrive, on the
  calling shard, with a second function, starting from an initial value.
- [`map()`]{index="`seastar::sharded`!`map()`"} runs it with every
  instance, and returns all the results, in a vector, in the order of
  the shards.

The program's main code, which we saw earlier with its calls to
`start()` and `stop()`, counts the words with one of the two functions
above, and then adds up the counts of all the instances with
`map_reduce0()`, `std::plus` and `counts`'s `operator+`:

```{.cpp include="examples/talking-between-shards/wordcount.cc" snippet="total"}
```

### One word at a time, or in batches

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
much as counting it. The receiving shards spend most of their time waiting
for shard 0, which does most of the work alone, and a program can't be
faster than its busiest shard. Dividing work over shards pays only when
the work is divided, not only the data it ends up in: here, the making of
the words must be divided too, as exercise 2 does.
[Chapter](#ch:designing-a-sharded-application) meets the same problem
in Shoal, where every shard must accept connections, not only own keys.

### Arguments made on each shard {index="`seastar::sharded_parameter`"}

`start()` passes its arguments to the constructor of every instance, in
one of three ways. An ordinary value is copied, so every instance gets
the same value. Another `sharded` object, wrapped in `std::ref()`, gives
each instance a reference to the same shard's instance of that service:
that's how most services get the services they depend on. And a
function wrapped in
[`sharded_parameter`]{index="`seastar::sharded_parameter`" .main} runs
on each shard, before that shard's instance is constructed, and its
result goes to the constructor. That is a value made on each shard, from
that shard's state, which the other two ways can't give. Two kinds of
values need it: a part of the same shard's instance of another service,
and an object that must be allocated on its own shard.

ScyllaDB's CQL layer shows the first kind. Its query processor needs to
know the database's schema: the keyspaces, the tables, and their
columns. It could take the same shard's instance of the database
service, with `std::ref()`, but then the CQL code would depend on the
database's class, and on everything that class depends on. Instead, the
code that starts the query processor passes it a `sharded_parameter`
whose function takes the shard's database and returns a small object
that only answers questions about the schema. The query processor's
code never mentions the database's class.

This section's example does the same, on a small scale. A `store` holds
the keys that belong to its shard, and a `reporter` reports how many
keys its shard has. The reporter doesn't take the store, only a function
that counts the keys:

```{.cpp include="examples/talking-between-shards/startup.cc" snippet="services"}
```

Each shard's reporter needs a function that counts the keys of its own
shard's store. The program makes it with `sharded_parameter`, whose
function takes the shard's store: the argument after the function,
`std::ref(stores)`, is passed to the function the same way that
`start()` would pass it to a constructor. The other argument of
`start()`, the name of what is counted, is an ordinary value, copied for
every instance:

```{.cpp include="examples/talking-between-shards/startup.cc" snippet="parameter"}
```

```
shard 0 has 3 keys
shard 1 has 2 keys
shard 2 has 2 keys
```

An ordinary argument couldn't do this. A counting function made on
shard 0 and copied would count shard 0's keys on every shard, reading
shard 0's store from the others, which [chapter](#ch:shared-nothing)
forbids. The same is true of the second kind of value, any object that
belongs to its shard, such as an `lw_shared_ptr`: copies of one made on
shard 0 would all change shard 0's reference count. Redpanda uses
`sharded_parameter` for both kinds: to give a service pointers to the
same shard's instances of other services, or to parts of them, and to
give it bindings to its shard's own copy of a configuration setting,
which tell it when the setting changes.

### A service with one instance {index="`seastar::sharded`!`start_single()`"}

Some services need only one instance: a coordinator, a cache of
something small, a client of a slow external service.
[`start_single()`]{index="`seastar::sharded`!`start_single()`"} starts
just one instance, on shard 0, and the other shards reach it with
`invoke_on(0, ...)`:

```{.cpp include="examples/talking-between-shards/startup.cc" snippet="single"}
```

```
shard 1 has an instance: false
shard 1 added a key on shard 0, which now has 1
shard 1 asks shard 1: sharded instance does not exist: store
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
`invoke_on(0, ...)`. It's
the same "only on shard 0" design, hidden behind the service's
interface, so that any shard can call `local()`.

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

## Limiting the calls a shard receives {index="`seastar::smp_service_group`"}

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
its senders together; each sender gets an equal share, and since each
needs at least one, a limit smaller than the number of possible senders
is quietly raised to it. A call joins
a group by passing it to `submit_to()`, or in an `smp_submit_to_options`,
which can also set a timeout for the wait for the group's unit, the
first step in [Figure](#fig:message-path). A group that's no longer
needed is destroyed with `destroy_smp_service_group()`.

The default group, which every call without one uses, has no limit.
Both large projects create groups of their own.

### Calls that wait for each other {index="deadlock!between shards"}

A group's limit comes with a danger, which Seastar's documentation
names: calls in a group must not *nest*. A call nests in another when
the function that the outer call runs, on the shard that received it,
makes the inner call, and waits for it. Nesting as such is allowed. The
danger is an inner call in the same group as an outer one.
This program makes two shards call each other at the same moment, and
each call, on the shard that receives it, makes a nested call back:

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
