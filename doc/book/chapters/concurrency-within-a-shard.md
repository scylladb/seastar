# Concurrency within a shard {#ch:concurrency-within-a-shard}

So far, Shoal's coroutines have worked side by side without knowing
about each other. Each client has its coroutine, which reads a request,
executes it, writes the reply and waits for the next one; while it waits,
the shard runs the others. The concurrency came from the clients: a
hundred clients, a hundred coroutines. In this chapter, code starts
concurrent work itself. It may start an operation and not wait for it,
leaving it to run [*in the background*]{index="background work" .main}; or it may start many operations
at once and wait for all of them, which is called [*fan-out*]{.index .main}, and combine
their results, which is called [*fan-in*]{.index .main}. Both are easy to write in
Seastar, and both have dangers that sequential code doesn't: work that
nobody waits for can fail with nobody noticing, outlive the data it uses,
or pile up without limit; and operations that run at the same time can
trip over each other.

Shoal's version 4 needs all of this. It adds Redis's [*Pub/Sub*]{index="Pub/Sub" .main}, in which
clients subscribe to channels, and a command of one client sends a
message to every client subscribed to a channel, by writing to their
connections, which may not be ready. And it makes `FLUSHALL ASYNC`,
which Shoal has accepted since version 2 but treated like `SYNC`, reply
at once, and delete the data in the background.

## Fibers {index="fiber"}

Each client's work, as described above, is sequential. Whenever
`handle_connection()` awaits a coroutine, such as `read_line()`, it
doesn't continue until that coroutine is done, so reading a request,
executing it, writing the reply, and looping to read the next one, all
run one step at a time, each step starting where the one before it
stopped. ([Figure](#fig:ping-steps) of
[chapter](#ch:how-seastar-runs-your-code) showed such steps, each a task
of its own, reading one `PING` and replying to it.) Seastar calls such a single thread of
execution a [*fiber*]{index="fiber" .main}, and Shoal serves each client
with one.

A shard runs many fibers, but only one task at a time, so its fibers
take turns, at the points where they wait ([Figure](#fig:fibers)): they
are concurrent, but not [parallel]{index="parallelism"}, the distinction
that [chapter](#ch:tutorial-introduction) made between the coroutines of
one shard, which only interleave, and the shards, which run in parallel.
A fiber does what a thread does in a multithreaded program, for much
less: it costs only the memory of its coroutine frames, and switching
between fibers costs a function call, so a shard can run hundreds of
thousands of them.

Other systems, such as Windows, Boost, Folly, Ruby and PHP, have fibers
too: threads of execution that the program schedules itself, switching
from one to another only where it chooses to. But there, each fiber has
a stack of its own, like a thread. A Seastar fiber has none: while it
waits, what it must remember is in the frames of its waiting coroutines.
Seastar keeps the word *thread* for the kind with a stack,
[`seastar::thread`]{index="Seastar thread"}, which
[chapter](#ch:threads) shows. A fiber has no type in Seastar: it's a
pattern of execution, not an object.

![The fibers of a shard that serves three clients. Each of the top four
rows is a fiber. The accept loop is a fiber, and it starts a separate
fiber for each client. Each fiber runs until it waits (dashed). The
bottom row is the shard's thread, which runs one fiber at a time, and
switches to another only where a fiber waits.](figures/fibers.svg){#fig:fibers}

A new fiber starts wherever code calls an asynchronous function and
doesn't await it: the operation then goes on by itself, alongside the code
that started it. The accept loop of [chapter](#ch:tutorial-introduction)
started a fiber for every connection this way: `(void)handle_connection(...)`
ignored the future that `handle_connection()` returned, instead of
awaiting it, and went straight back to accepting connections. This
chapter is about such places, where code splits into several fibers, and
where it waits for them to join again.

## Work in the background {index="background work"}

### Starting it

`shoal_main()` is still the one that [chapter](#ch:coroutines) wrote,
which creates the store, listens for clients, and serves each one in the
background. The only change is that `handle_connection()` gets one more
argument, `ps`, which holds the Pub/Sub subscriptions that this chapter
adds; we'll see it when we get to Pub/Sub, below. The loop starts each
connection's fiber:

```{.cpp include="shoal/v4/main.cc" snippet="accept-loop"}
```

Calling `handle_connection()` without a `co_await` before it is all it
takes to run it in the background. The `(void)` is there because `future` is declared
`[[nodiscard]]`, and the compiler would otherwise warn that we ignored a
future, which is usually a mistake ([chapter](#ch:futures-and-promises)).
But writing `(void)` only silences the warning. It doesn't answer the
questions that the warning is about: where the operation's errors go,
whether the data it uses lives long enough, and who waits for it to end.

### Where its errors go

Suppose `handle_connection()` fails, because the client reset the
connection. The accept loop attaches `handle_exception()` to its future,
which we'll come to in a moment. Without it, the future would fail, and,
since nobody holds that future, Seastar would log "Exceptional future
ignored" ([chapter](#ch:futures-and-promises)). The warning would tell us
that something failed, but not which client, and it would look like a bug,
which is what it usually means.

Why not a `try` and a `catch` around the call?

```cpp
    try {
        (void)handle_connection(db, ps, std::move(socket));
    } catch (...) {
        // Never reached when the connection fails.
    }
```

Because the call doesn't fail. A coroutine doesn't throw to its caller:
when it fails, its future fails, and the exception is thrown only where
someone awaits that future ([chapter](#ch:coroutines)). The call returns a
future at once, before the connection has done anything, so there is
nothing to catch yet; and the `catch` block is long gone when the
connection fails. An error in the background can only be handled by
something attached to the future itself, or by the background operation
itself.

[`handle_exception()`]{index="`seastar::future`!`handle_exception()`" .main}
is the first way. It's a method of `future`, which takes a function, and
returns a new future. When the original future becomes available, the new
one gets the same value; when it fails, `handle_exception()` calls the
function with the exception, as a `std::exception_ptr`, and the new future
gets whatever the function returns. For a `future<>`, the function
returns nothing, so the new future becomes available as if nothing had
failed. Shoal's function logs the error with the client's address, which
it captured, and so turns a failure into a log message. We still drop the
new future, but it can't fail anymore (unless the function itself
throws), so nothing is ignored.

The second way is to give the background work a coroutine of its own, which
awaits the operation and catches its errors:

```cpp
future<> serve_in_background(store& db, pubsub& ps, connected_socket s,
                             socket_address address) {
    try {
        co_await handle_connection(db, ps, std::move(s));
    } catch (...) {
        shoal_log.warn("connection with {} failed: {}", address,
                       formattable(std::current_exception()));
    }
}
```

and to call it with `(void)serve_in_background(...)`. That's a little more
code, but it's an ordinary coroutine, which can do anything that a
coroutine can, such as clean up with `co_await` after the error (outside
the `catch` block). Both ways are common in Seastar code. What matters is
that every background operation handles its own errors, because nobody
else will.

### Who owns its data

A background operation keeps running after the code that started it has
moved on, so it must not use that code's local variables.
`FLUSHALL ASYNC`[]{index="Redis commands!`FLUSHALL`"} shows the danger. Redis's `FLUSHALL` deletes all the
keys, and with `ASYNC`, it replies at once and frees the memory in the
background. Shoal's `FLUSHALL` from [chapter](#ch:how-seastar-runs-your-code)
already moved the data out of the store with `std::exchange()`, and then
destroyed it gradually, to avoid a stall. For `ASYNC`, it now hands the
data to a new coroutine, `destroy_gently()`, which destroys it in the
background, and replies without waiting for it:

```{.cpp include="shoal/v4/commands.cc" snippet="flushall-async"}
```

`destroy_gently()` takes the data *by value*, and that matters. The old
data is the local variable `old`, which disappears when `flushall()`
returns, long before the background work is done. Calling
`clear_gently(old)` in the background, which takes a reference, would
leave the background coroutine with a reference to destroyed memory.
Instead, `std::move(old)` moves the data into the parameter `data`, which lives in
`destroy_gently()`'s frame for as long as the coroutine runs. The
coroutine owns the data, and the data dies with it. This is the general
rule: a background operation must own what it uses, or use what is
guaranteed to outlive it. Passing by value, as here, is the simplest way;
[chapter](#ch:object-lifetime) shows the others, and why references,
`this` and lambda captures are so easy to get wrong.

Why not simply pass `std::move(old)` to `clear_gently()`? It doesn't compile:
`clear_gently()` takes a `Container&`, which can't refer to what
`std::move()` returns. And a `clear_gently()` that took a `Container&&`
would compile, but be just as wrong. `std::move()` doesn't move anything:
it only marks `old` as an object that may be moved from, so the parameter
would still refer to `old` itself. The data moves only when a new object
is made from it, as `destroy_gently()`'s parameter `data` is.

(Is it safe to drop `destroy_gently()`'s future? It can't fail: erasing
from a hash table doesn't throw, and neither does yielding. If it could,
we would need `handle_exception()` here too, or a `try` and `catch`
inside `destroy_gently()`.)

Redis's documentation adds a detail, which Shoal gets right without
trying: "an asynchronous FLUSHALL command only deletes keys that were
present at the time the command was invoked. Keys created during an
asynchronous flush will be unaffected." Shoal's store is empty from the
moment `std::exchange()` ran, and new keys go into the new, empty store,
while the background coroutine destroys only the old one. A test sets a
key right after `FLUSHALL ASYNC`, and checks that it survives.

### What waits for it

Nothing waits for a background operation, and that has two consequences.
First, nothing limits how many there are. A client that sends
`FLUSHALL ASYNC` over and over, each time after loading new data, starts
a new background destruction each time, and if it loads data faster than
they free it, memory grows without limit. Shoal's background destructions
are cheap and quick, so this is mostly theoretical here, but a server
that starts background work in response to requests must ask how much of
it can pile up. [Chapter](#ch:coordination) shows how to limit it, with
semaphores.

Second, nothing waits for background operations when the program stops.
Today, Shoal stops by being killed, which ends everything at once. A
server that stops cleanly, as Shoal will in
[chapter](#ch:clean-shutdown), must first wait for the operations it
started: otherwise they go on using objects that the shutdown destroys.
[Chapter](#ch:coordination) introduces the gate, which keeps track of
background operations, and lets their owner wait for them to end, and
[chapter](#ch:clean-shutdown) uses it to stop Shoal. Until then, every background operation in this book
is one that Shoal can afford to abandon.

## Many operations at once

The other way to split a fiber is to start several operations, and wait
until all of them are done. [Chapter](#ch:futures-and-promises) showed the
idea: a coroutine calls several asynchronous functions, keeps their
futures, and only then awaits them, so that the operations proceed at the
same time. Seastar has functions that do this for you, for a few
operations or for a whole range of them, and that combine their results.
To show them, this chapter's example program is a benchmark for
Redis's Pub/Sub, which Shoal version 4 adds. So first, what Pub/Sub is.

### Redis's Pub/Sub {index="Pub/Sub"}

Pub/Sub, short for *publish and subscribe*, lets Redis's clients send
messages to each other, through the server. A client
[*subscribes*]{index="Pub/Sub!subscriber"} to channels, named by any string, and
from then on receives every message that any client
[*publishes*]{index="Pub/Sub!publisher"} to them. The first client is called a
subscriber, and the second a publisher. Here it is, with Shoal version 4
listening on port 6390, as in [chapter](#ch:coroutines) (with
`valkey-server`, it looks the same):

```
$ valkey-cli -p 6390 SUBSCRIBE news
1) "subscribe"
2) "news"
3) (integer) 1
```

This `valkey-cli` doesn't exit: it stays connected, and prints each
message as it arrives, until you press Ctrl-C. In another terminal, a
`PUBLISH` replies and exits, like any other command:

```
$ valkey-cli -p 6390 PUBLISH news "Shoal 4 is out"
(integer) 1
```

and the first terminal shows the message:

```
1) "message"
2) "news"
3) "Shoal 4 is out"
```

[`SUBSCRIBE`]{index="Redis commands!`SUBSCRIBE`"} confirms each channel
with an array of three elements: the word `subscribe`, the channel, and
the number of channels the client is now subscribed to. A message arrives
as a similar array, starting with the word `message`. `PUBLISH`
[]{index="Redis commands!`PUBLISH`"}replies with the number of clients
that it sent the message to. And
[`UNSUBSCRIBE`]{index="Redis commands!`UNSUBSCRIBE`"}, with channels or,
to leave them all, without, confirms each channel the way `SUBSCRIBE`
does. Channels have nothing to do with the keys: publishing doesn't store
anything, and a message published to a channel without subscribers is
lost.

So the publishers and the subscribers are all clients of the server,
which only passes each published message on, to the clients subscribed
to its channel. A benchmark for Pub/Sub, such as this chapter's
example program, `pubsub-bench`, makes many connections to the server:
it subscribes many of them to a channel, publishes messages to the
channel through one or more others, and counts how many messages the subscribers
receive, and how fast. We use it to measure Shoal below.

### A few operations

[`coroutine::all`]{index="`seastar::coroutine::all`" .main}, from
`<seastar/coroutine/all.hh>`, runs a few
operations at once, given as functions that return futures, and awaits
them all. Awaiting it gives back a tuple of their results: one for each
operation whose future has a value, so none for a `future<>`.
`pubsub-bench` gives it two functions, two lambdas, to run the
subscribers and the publishers at the same time:

```{.cpp include="examples/concurrency-within-a-shard/pubsub-bench.cc" snippet="run-all"}
```

The first lambda counts what the subscribers receive (we'll see how in
a moment), and its future holds a number; the second runs the publishers,
with `parallel_for_each()`, and its future holds nothing. So the tuple
that awaiting `coroutine::all` gives back has one element, the number of
messages received, which the structured binding `auto [received]`
unpacks. If an operation
fails, `coroutine::all` still waits for the others, and then throws its
exception; if several fail, it throws one of their exceptions.

Seastar's older functions [`when_all()`]{index="`seastar::when_all()`"}
and [`when_all_succeed()`]{index="`seastar::when_all_succeed()`"}, from
`<seastar/core/when_all.hh>`, do the same, and also accept futures directly, instead of functions that return
them. They differ in how they report failures. `when_all_succeed()` is
like `coroutine::all`: its future holds the values, or fails with one of
the exceptions. `when_all()` never fails: its future holds a tuple of the
original *futures*, each available or failed, for code that needs to know
which of the operations failed. In a coroutine, `coroutine::all` is the
one to use, and its documentation notes that it's safe to give it lambda
coroutines, which [chapter](#ch:object-lifetime) explains is not true of
every function.

The two lambdas use two more of Seastar's functions,
`parallel_for_each()` and `map_reduce()`, which run an operation for
every element of a range. The next two sections explain them.

### A range of operations

The second lambda runs the publishers with Seastar's function
[`parallel_for_each()`]{index="`seastar::parallel_for_each()`"}, from
`<seastar/core/loop.hh>`, which runs an operation for each element of a range, all at once. It calls the
function for every element, immediately, without waiting, and returns a
future that is available when all the operations are done. Here, the
range is the numbers from 0 to the number of publishers, and the function
runs one publisher.

In a coroutine, the same is written with
[`coroutine::parallel_for_each`]{index="`seastar::coroutine::parallel_for_each`"},
from `<seastar/coroutine/parallel_for_each.hh>`:

```cpp
co_await coroutine::parallel_for_each(range, [] (auto& element) {
    return an_asynchronous_function(element);
});
```

This looks like a function call, but `coroutine::parallel_for_each` is a
class, like `coroutine::all`: the expression creates an object, which can
only be awaited, so it's only for coroutines. The second lambda isn't a
coroutine, and must return a future for `coroutine::all` to await, so it
uses the function instead. Shoal uses `coroutine::parallel_for_each`
below, to send a message to many subscribers at once.

Starting everything at once is sometimes too much. `pubsub-bench` connects
many subscribers to the server, by default a hundred. Connecting them one
at a time would be slow, since every connection takes a round trip to the
server. But starting thousands at once could overflow the server's queue
of connections that it hasn't accepted yet, if it doesn't accept them as
fast as they arrive: its
[*backlog*]{index="listen backlog"}. A server chooses its length when it
starts listening (a Seastar server's is 100, unless it asks for more), and
Linux drops a request that doesn't fit, so that the client tries again
only a second later. (See [chapter](#ch:networking) for more about the
backlog.) So
`pubsub-bench` sets up only a few at a time, with
[`max_concurrent_for_each()`]{index="`seastar::max_concurrent_for_each()`" .main},
also from `<seastar/core/loop.hh>`:

```{.cpp include="examples/concurrency-within-a-shard/pubsub-bench.cc" snippet="connect-all"}
```

It starts the function for the first `opts.max_connecting` elements (the
option `--max-connecting`, ten by default), and then, whenever
one of them finishes, starts the next, so that that many are in progress
at any time, until all have finished. Unlike `parallel_for_each()`, it has only
one form, a function that returns a future, which a coroutine awaits
like any other; so does `map_reduce()`, below.

The range is the vector `subscribers`, with one element, a slot, for
each subscriber, created before any of them connects; `subscribe()`
connects one subscriber, and fills its slot. Until now, our programs only
accepted connections; a client makes one with Seastar's
[`connect()`]{index="`seastar::connect()`" .main}, also from
`<seastar/core/seastar.hh>`, which returns a future for the
`connected_socket`. `pubsub-bench` keeps the socket together with its
two streams, which [chapter](#ch:coroutines) explained must be created
once per connection, in a `connection`:

```{.cpp include="examples/concurrency-within-a-shard/pubsub-bench.cc" snippet="connect-to"}
```

`make_ipv4_address()` makes the server's address, here a port on this
machine. A `connection` can't exist without its socket, so the vector
can't hold `connection`s from the start: it holds a
`std::optional<connection>` for each subscriber, which starts out empty.

`subscribe()` connects, subscribes, and puts the new connection in its
slot. It uses two more small helpers from
`pubsub-bench.cc`, not shown here. `command()` writes a request in RESP,
the protocol of Redis, from its words. And `expect()` reads as many bytes as the reply it expects, and throws if
they aren't that reply; here, it's the confirmation of the
subscription, the array of `subscribe`, the channel and 1 that we saw
above, as RESP sends it:

```{.cpp include="examples/concurrency-within-a-shard/pubsub-bench.cc" snippet="subscribe"}
```

The vector `subscribers` is a local variable of the coroutine that
awaits all the calls, so it outlives them. That matters, because
`max_concurrent_for_each()` keeps iterating over the range while the
calls run, so the range must exist until the future it returns is available.
Its documentation warns that "this does not allow the range to refer to
stack objects", which, in a coroutine, isn't a problem: the coroutine's
local variables live in its frame, not on the stack.

### Combining the results

When every operation produces a result, and what we want is a
combination of them, such as their sum,
[`map_reduce()`]{index="`seastar::map_reduce()`" .main}, from
`<seastar/core/map_reduce.hh>`, runs a function
on every element of a range, all at once, and combines the results as they
arrive. `pubsub-bench` gives every subscriber a fiber that receives the
messages it expects, and returns how many it got:

```{.cpp include="examples/concurrency-within-a-shard/pubsub-bench.cc" snippet="receive"}
```

and the first lambda given to `coroutine::all` above runs `receive()` on
every subscriber, and adds up the counts, starting from zero, with
`std::plus`. (If a subscriber's connection closes before it receives all
its messages, its read fails, and so does the whole benchmark.) That's
fan-out and fan-in in one call. `map_reduce()` has other forms, which take
a [*reducer*]{index="`seastar::map_reduce()`!reducer"}, an object that
accumulates the results, such as Seastar's `adder`; [Part](#part:many-cores) uses
`map_reduce()` across shards, where it's at its most useful.

### When concurrency helps

Running operations at the same time makes them faster only if they spend
time waiting. A shard runs one task at a time, so ten operations that
only compute take as long together as one after another, or a little
longer, because of the futures and coroutines involved. But ten
operations that each wait a millisecond for the network take about a
millisecond together, instead of ten.

That's why Shoal's version 4 is about Pub/Sub, and not about executing
each client's requests concurrently. A client that pipelines its requests
sends many before reading the replies, and it may seem natural to execute
them at the same time. But Shoal's commands never wait: on one shard, with
its data in memory, every command runs to the end as soon as it starts.
Executing them concurrently would overlap nothing, and would only add the
cost of futures, and of keeping the replies in the right order. That
changes when commands start waiting, for other shards or for the disk, in
[Part](#part:many-cores) and [Part](#part:io). Pub/Sub, on the other hand,
waits for something already: for other clients' connections.

## Shoal, version 4 {index="Shoal!version 4"}

Version 4 adds Pub/Sub, and makes `FLUSHALL ASYNC` delete the data in
the background.[^shoal-v4] We saw `FLUSHALL ASYNC` above; the rest of
this section is about Pub/Sub.

[^shoal-v4]: The source of this version is in `shoal/v4/`.

### Publish and subscribe {index="Pub/Sub!in Shoal"}

In Pub/Sub, as explained above, clients subscribe to channels and
receive every message that other clients publish to them. Two more
details of Redis's Pub/Sub matter to Shoal.

While a client is subscribed to anything, it's in a mode of its own: Redis
sends it messages whenever they are published, so it may only send
commands that are about subscriptions, and `PING`, whose reply in this
mode is an array, `pong` and an empty string, so that a client can tell it
from a message. Any other command gets an error. (Redis's newer protocol,
RESP3, which Shoal doesn't support, lets subscribed clients send any
command.)

Redis's documentation promises little about delivery. Messages are
delivered [at most once]{index="Pub/Sub!delivery guarantees"}: a message that a subscriber can't receive, for
example because it disconnected, is lost. But each subscriber receives
the messages in the order they were published. And, the documentation
says, a subscriber that doesn't read its messages fast enough is
disconnected: Redis keeps the messages that it couldn't send yet in
memory, and once they reach 32 MB, it closes the connection.

Shoal keeps the subscriptions in a class of their own, `pubsub`, in a
new source file, `pubsub.cc`, with a hash table that maps each channel to the set of its
subscribers:

```{.cpp include="shoal/v4/pubsub.hh" snippet="pubsub"}
```

`shoal_main()` creates one, next to the store, and passes it to the
`handle_connection()` of every client:

```{.cpp include="shoal/v4/main.cc" snippet="create-pubsub"}
```

The commands of a client that subscribes, or that is subscribed, go to
`pubsub::execute()`; everyone else's go to `execute()`, as before. The
class's enum `delivery` chooses among three ways to send a message to
the subscribers, `sequential`, `parallel` and `background`. We'll see
why there are three, and compare them, in "Delivering a message", below.

### Writing to another client's connection

Until now, only a client's own fiber wrote to its connection. Pub/Sub
changes that: `PUBLISH` runs in the publisher's fiber, and writes the
message to the subscribers' connections. The obvious way to write
`PUBLISH` is a loop over the subscribers, which writes the message to each
one's output stream:

```cpp
    for (client* s : subscribers) {
        co_await s->out().write(message);
        co_await s->out().flush();
    }
```

It works, in the simple tests. But the subscriber's connection now has two
writers that know nothing about each other: its own fiber, which still
answers its `PING`s and its `SUBSCRIBE`s, and the publisher's. And with
many publishers, it has many. If one of them waits in `write()` or
`flush()`, because the connection's buffers are full, another may start
writing in the meantime. An `output_stream<char>` doesn't support that: its
`write()` and `flush()` must not be called again before the previous call's
future is available. Doing it is a bug, with unpredictable results.

We tried it. In a copy of Shoal that writes like this, two publishers
sent 100 KB messages to the same subscriber, which read slowly, and sent
`PING`s of its own. Built against a `debug`-mode Seastar, Shoal stopped
within a second (shortened here):

```
ERROR ... seastar - wrapped_iovecs are re-populated live, at: ...
==96766==ERROR: AddressSanitizer: heap-use-after-free on address ...
READ of size 8 at 0x7b58a51f86e8 thread T0
    #0 ... in std::vector<iovec, std::allocator<iovec> >::clear() ...
    #1 ... in operator() src/net/posix-stack.cc:108
...
==96766==ABORTING
```

Seastar's network code noticed that a write had started while the
previous one was still sending, and complained; then AddressSanitizer,
which [chapter](#ch:object-lifetime) shows, found the two writes using
memory that one of them had already freed, and stopped the program.
(`debug` mode also has an assertion made for exactly this, that a write
to a stream doesn't start before the previous one has finished. In an
earlier run of ours, with another build of Seastar and a slightly
different copy of Shoal, it was the assertion that stopped Shoal.) In `release`
mode, Shoal went on running, and only logged the same complaint, and the
connections failed. Neither mode's network code is something to rely on:
in general, overlapping writes are a bug that `release` mode won't
report, and this is exactly the kind of bug that
[chapter](#ch:testing)'s `debug` mode is for.

### One write at a time

So every write to a subscriber's connection must wait until the previous
one has finished, whoever wrote it. Shoal does this with a small class,
`client`, which represents a connection, and through which all of its
writes go, the client's own replies and the messages of the publishers
alike:

```{.cpp include="shoal/v4/pubsub.hh" snippet="client"}
```

The idea is to make each write wait for the previous one's future. The
client keeps the future of its last write, as a `shared_future` from
[chapter](#ch:futures-and-promises), since it may be waited for by more
than one: by the next write, and by whoever wants to know that the writes
are done. `send()` starts a write, which first waits for the previous one,
and makes it the last:

```{.cpp include="shoal/v4/pubsub.cc" snippet="send"}
```

`send()` doesn't wait for anything. It calls `write_after()`, which awaits
the previous write before it writes, and keeps its future, so that the
next write waits for this one. If the previous write is already done, the
new one starts at once, inside `send()`; otherwise it waits its turn. The
writes form a chain, which runs in the order the sends were made, so
that a subscriber gets its messages in the order they were published, as
Redis promises. `write()` is `send()` followed by waiting for this write
to finish, and `drain()` waits for all the writes sent so far. And
`_pending` and `_total_pending` count the memory used by messages that
were sent but not written yet, this client's and all the clients'
together, for reasons we'll see below. Whether the write succeeds or
fails, `write_after()` subtracts its message's cost at the end, so that a
failed write doesn't stay counted forever. It then passes the failure
on with `coroutine::return_exception_ptr()`, which, as
[chapter](#ch:coroutines) showed, fails the coroutine without throwing
the exception again.

If a write fails, because the connection broke, the writes chained after
it fail too, without trying to write, when they await the failed future.
Nobody needs to handle these failures one by one: they all end in the
last write's future, which the connection's fiber awaits when it ends.

A subscribed client's own replies go through the same chain.
`pubsub::execute()`, not shown, chooses the command, and refuses the
commands that the subscribed mode doesn't allow; each command writes its
replies with `write()`, or with `send()` and then `drain()`. `SUBSCRIBE`,
for example, sends a confirmation for each channel (an array that
`confirmation()` formats), and then waits for them all to be written:

```{.cpp include="shoal/v4/pubsub.cc" snippet="subscribe"}
```

`UNSUBSCRIBE` works the same way. A client that isn't
subscribed can't receive messages, so its commands write directly to the
stream, as before, without the cost of the chain. The connection's loop
chooses between the two:

```{.cpp include="shoal/v4/main.cc" snippet="serve"}
```

When a client unsubscribes from its last channel, messages published
before that may still be on their way to it, in the chain. So before the
loop goes back to writing directly, it waits for them with `drain()`.

### Delivering a message

Now `PUBLISH` can write to its subscribers safely. After checking its
arguments, `pubsub::publish()` copies the list of the channel's
subscribers:

```{.cpp include="shoal/v4/pubsub.cc" snippet="subscribers"}
```

The copy is needed for the same reason as in
[chapter](#ch:how-seastar-runs-your-code)'s `KEYS`: if we iterated over
the set itself, and waited in the middle, a client that subscribed or
unsubscribed while we waited could change the set under our iterator.

Then it delivers the message, which it has formatted as RESP sends it,
in `message`. Shoal can do that in three ways, so that we can compare
them ([Figure](#fig:delivery)). They are the three cases of a `switch` on
`_delivery`, the `delivery` that we saw in the class, which the option
`--pubsub-delivery` sets. After the `switch`, `PUBLISH` replies with the
number of subscribers that it sent the message to.

![The three ways to deliver a message, when one of three subscribers
never reads. An arrow is a write that `PUBLISH` started, and a small
block is a message waiting in a subscriber's chain. One after the other,
`PUBLISH` is stuck at the slow subscriber, and the subscribers after it
get nothing. All at once, the other subscribers get the message, but
`PUBLISH` is still stuck. In the background, `PUBLISH` replies at once,
and the slow subscriber's messages pile up until it's
disconnected.](figures/delivery.svg){#fig:delivery}

The first way, `delivery::sequential`, writes to the subscribers one
after the other, and waits for each write before the next:

```{.cpp include="shoal/v4/pubsub.cc" snippet="sequential"}
```

Even with the copy, waiting has a consequence: by the time we get to a
subscriber, it may have disconnected, and its `client` object may have
been destroyed. So before writing to a subscriber, the loop checks that
it's still subscribed. We'll see in a moment why a client that is still
subscribed is safe to write to.

The second way, `delivery::parallel`, writes to all the subscribers at
once, with `coroutine::parallel_for_each`, and then waits for all the
writes:

```{.cpp include="shoal/v4/pubsub.cc" snippet="parallel"}
```

Here, every subscriber gets its write before `coroutine::parallel_for_each`
waits for anything, so the problem of the first way doesn't arise: all the
subscribers are still subscribed when their writes start. Each write's
errors are handled with `handle_exception()`, as in the accept loop: a
subscriber whose connection fails reports that itself, when its own fiber
finds the failed write.

This starts a write to every subscriber at once, however many there
are, while `pubsub-bench` limited its connections with
`max_concurrent_for_each()`. Should this be limited too? Each write keeps
its own copy of the message until it's done, so with many subscribers,
a limit would save memory. But it would also make subscribers wait for
slow ones: with a limit of ten, ten subscribers that don't read would
stop the delivery to all the others, as the first way did. Exercise 3
measures what a limit costs.

The third way, `delivery::background`, is Redis's: send the message to
every subscriber, and reply to the publisher without waiting for any of
the writes, which complete in the background:

```{.cpp include="shoal/v4/pubsub.cc" snippet="background"}
```

Here, `send()` only adds the message to each subscriber's chain. Background
delivery has the dangers of every background work, and this code deals
with each of them. The message's errors end in the subscriber's chain,
which its own fiber awaits. The message is owned by the chain: `send()`
takes its own copy of the message, by value, into `write_after()`'s frame.
And the messages that subscribers haven't received yet can't pile up
without limit. `PUBLISH` disconnects a subscriber whose unwritten messages
use more than 32 MB of memory, as Redis does (`--pubsub-max-pending`
changes the limit); and when all the subscribers' unwritten messages
together use more than a quarter of the shard's memory, it disconnects
those that are behind. We'll see below why both limits are needed.
`forget()` removes the subscriber from all its channels, and
`disconnect()` shuts down its socket, which makes its pending writes fail,
and its fiber's next read end, so that the fiber ends, and cleans up
after itself.

How do the three compare? The difference shows with a subscriber that
never reads. We subscribed a client that read nothing, and another that
read everything, and published messages of 1 MB:

| Delivery | `PUBLISH`es done | Slowest `PUBLISH` | The reading subscriber got |
|---|---|---|---|
| one after the other | 2, then stuck | over 3 s, when we gave up | 3 messages |
| all at once | 2, then stuck | over 3 s, when we gave up | 3 messages |
| in the background | all 60, in 0.11–0.14 s | 19–21 ms | all 60 |

(three runs each, which agreed).

With the two ways that wait, one after the other and all at once, about
two messages fit in the kernel's buffers,
and then the third `PUBLISH` waited for the
slow subscriber to read, which it never did: the publisher was stuck,
until our client gave up after three seconds. One after the other, the
other subscribers would also wait for their messages, if they came after
the slow one in the loop. In the background, `PUBLISH` never waited; the
slow subscriber was disconnected after 33 messages, when its unwritten
messages passed 32 MB, and the other subscriber received all 60.

Most subscribers do read, and then the difference is speed. We ran
`pubsub-bench` with a hundred subscribers, and a publisher that sent a
million messages of 100 bytes, 1,024 `PUBLISH` requests at a time before
reading their replies, with Shoal and the benchmark on two of the
laptop's fast cores. In three runs each:

| Delivery | Messages delivered per second |
|---|---|
| one after the other | 9,230,000–9,680,000 |
| all at once | 11,220,000–11,360,000 |
| in the background | 11,290,000–11,360,000 |

When the subscribers keep up, the three ways are close. The first, the
only one that waits for each write before starting the next, is the
slowest, by about a sixth. Neither program was the limit: in separate runs
that measured it, Shoal was busy about four fifths of the time, and the
benchmark less than half, so what limits these numbers is how the two
exchange data, not how fast either computes. With one `PUBLISH` at a time
instead, every way delivered only about 480,000
messages a second, and both programs were mostly idle: every message then
waited for a round trip between the publisher and Shoal. As in
[chapter](#ch:coroutines), pipelining matters as much as anything that
happens inside the server.

So is background delivery simply the best of the three? It never gets
stuck, and it's the fastest, but it has a price, which these two
experiments didn't show. In the two ways that wait, a subscriber that reads
slowly slows the publisher down to its own pace, because `PUBLISH` waits
for its write. Pushing back on the producer of work this way, until the
consumers catch up, is called
[*backpressure*]{index="backpressure" .main}, and it's what keeps a
system from accepting more work than it can finish. Background delivery
has none: the publisher never waits, so when subscribers fall behind,
their messages pile up in Shoal's memory, and in the end, Shoal has no
remedy left but to disconnect them, and lose their messages, as the next
section shows. Redis chose that, and so does Shoal's version 4, by
default. The other two ways remain available with `--pubsub-delivery`,
to experiment with. Shoal's version 6, in [chapter](#ch:coordination),
will get both: it will deliver in the background, but make a publisher
wait while the messages that are waiting for subscribers use too much
memory.

### Limiting the right thing

When we first wrote background delivery, for this chapter, it had only
the limit for each subscriber, and counted only the bytes of the
messages. With one publisher, as in the measurements above, the hundred
subscribers kept up with it, and nothing piled up. Then we ran
`pubsub-bench` with two publishers, both publishing as fast as they
could, and Shoal stopped within seconds:

```
terminate called after throwing an instance of 'std::bad_alloc'
  what():  std::bad_alloc
Aborting on shard 0, in scheduling group main. Backtrace: ...
```

It had run out of memory. Two publishers sent messages faster than the
hundred subscribers could read them, so every subscriber fell behind, and
the limit counted the wrong thing in two ways. First, it limited each
subscriber, but not all of them together: a hundred subscribers, each
allowed 32 MB, may hold 3.2 GB, more than the 1 GB we gave Shoal. Second,
a message waiting to be written costs more than its bytes. We published
200,000 messages to a subscriber that read nothing, and measured how much
Shoal's memory grew: by 713 bytes for every message of 100 bytes, and
by 1,919 for every message of 1,000 bytes. That's 613 and 919 bytes more
than the messages themselves. Part of the difference is the cost of
waiting, about the same for every message: the frame of `write_after()`,
the `shared_future`, and the allocations behind them. The rest grows with
the message: what waits is the message in RESP, a few dozen bytes
longer, and Seastar's allocator rounds each allocation up to one of its
sizes, which can add up to a quarter to a large one.

So version 4 counts each waiting message as its size plus 512 bytes, and
limits the total, as well as each subscriber. That's too little, by about
a hundred bytes for small messages and four hundred for large ones, but
good enough to keep Shoal alive; [chapter](#ch:coordination)'s version 6 will count the rounding too.

We ran the same benchmark, with two publishers, against version 4 with
these limits. This time, Shoal didn't run out of memory: it disconnected
all the subscribers, which had all fallen behind, and went on running. That's the right outcome for a server that is asked to
deliver more than its clients can receive, but disconnecting clients is a
crude way to get it. [Chapter](#ch:coordination) has better tools: a
semaphore that makes publishers wait when too much is pending, which
slows them to the pace of their subscribers, and a queue, which keeps a
waiting message without a coroutine frame and a future of its own. The
lesson stays the same: work that piles up in the background must be
limited, by what it really costs.

### When a client goes away

A subscriber's `client` object is used by other fibers, the publishers',
whose writes are in its chain. So when its connection ends, it must not
simply be destroyed: the publishers' writes, in the background, would then
write to a stream that no longer exists. `handle_connection()` makes sure
that doesn't happen, however the connection ended:

```{.cpp include="shoal/v4/main.cc" snippet="handle-connection"}
```

First, `forget()` removes the client from all its channels, so that no
publisher sends it anything more. Then `drain()` waits for the writes that
were already sent. Only then are the client and the stream destroyed. And
this happens whether `serve()` ended normally or with an exception, which
is why the exception is only remembered, and returned at the end, with
`coroutine::return_exception_ptr()`.

When we discussed delivery one after the other, `delivery::sequential`,
we said that its loop may write to any client that is still listed as
subscribed to the channel. Now we can see why. A client that is still
on the list hasn't reached `forget()`, so it hasn't been destroyed; and
a client that has reached `forget()` is no longer on the list, so the
loop's check skips it. The rule behind all of this is the
same as for `FLUSHALL ASYNC`: whatever a background operation uses must
live until the operation ends. Here, the object is shared by several
fibers, so it must live until all of them are done with it, and its owner
waits for them, instead of letting them own it.
[Chapter](#ch:object-lifetime) shows another way, in which the fibers
share the ownership, with reference counting.

### Tests

Rather than rely on Redis's documentation, which is sometimes vague,
Shoal's tests run the same requests against both Valkey and Shoal, and
check the exact replies. A test that passes on both shows that the two
servers behave the same. As in [chapter](#ch:coroutines), the script
`run`, in `shoal/test/`, runs them, given the version, 4, against the
Shoal built in `shoal/v4/build`, or, with `--valkey`, against a
`valkey-server`:

```
$ cd shoal/test
$ ./run 4 -q
...................................... [100%]
38 passed in 3.14s
$ ./run 4 --valkey -q
...................................... [100%]
38 passed in 3.91s
```

The 38 tests include those of the earlier versions, unchanged, in
`test_v1.py`, `test_v2.py` and `test_v3.py`, so that each version is also
checked not to break what the earlier ones did. Version 4's 17 new
tests, in `test_v4.py`, cover Pub/Sub and `FLUSHALL ASYNC`. Among other
things, they check that a subscriber receives a thousand messages in the order
they were published, and that a subscriber that never reads doesn't hold
up the publisher.

That last test taught us something about Valkey. Its documentation says
that a subscriber's unsent messages are limited to 32 MB, and Valkey
reports this limit when asked (`CONFIG GET client-output-buffer-limit`).
But when we published a hundred messages of 1 MB to a subscriber that read
nothing, Valkey (version 9.0.6) didn't disconnect it, and reported that its output buffer
held only about 1 MB. Whatever Valkey counts against the limit, it isn't
the published messages' full size, as its documentation suggests; we
didn't look into why. So the test
requires only what both servers do: that the publisher isn't held up, and
that a subscriber that was disconnected isn't counted anymore.

## Exercises

1. Make a copy of Shoal version 4 in which `client::write()` writes
   directly to the output stream, without the chain, as in the first
   attempt above. Run version 4's tests against it: do they pass? Then
   build it against a `debug`-mode Seastar, and write a test that makes
   two writes overlap, with two publishers and a subscriber that reads
   slowly. What happens?

2. Shoal doesn't implement Redis's `PSUBSCRIBE`, which subscribes to all
   the channels that match a pattern, such as `news.*`, with the same
   patterns as `KEYS`. Add it, and its `PUNSUBSCRIBE`, using
   `glob_match()` from [chapter](#ch:how-seastar-runs-your-code) (it's in
   an anonymous namespace in `commands.cc`, so you'll need to declare it
   in a header first). Find out
   from Valkey what a pattern subscriber receives (a `pmessage`, with the
   pattern), and what `PUBLISH` counts when a client is subscribed to a
   channel and also to a pattern that matches it. Write the tests first,
   and check them against Valkey.

3. `PUBLISH`'s `delivery::parallel`, with `coroutine::parallel_for_each`,
   writes to all the subscribers at once. With a hundred thousand subscribers, that's a
   hundred thousand writes in progress, each holding its own copy of the
   message. Change it to use `max_concurrent_for_each()`, with at most a
   hundred writes at a time, and compare the two with `pubsub-bench` and
   many subscribers. Does the limit change the time a `PUBLISH` takes?
   The memory Shoal uses?

4. Run `pubsub-bench` with a thousand subscribers, and `--max-connecting`
   set to 1, 50 and 1,000. How long does connecting take in each case?
   The kernel counts the connections that it dropped because the backlog
   was full in `TcpExtListenOverflows`, which `nstat` shows. Did any
   overflow, even though Shoal had nothing else to do? Then freeze Shoal
   with `kill -STOP`, start `pubsub-bench`, and resume Shoal with
   `kill -CONT` a third of a second later. While it's frozen, `ss -ltn`
   shows the backlog of Shoal's listening socket, in the column `Send-Q`,
   and how many connections wait in it, in `Recv-Q`. Now how long does
   connecting take, with 50 connections at a time, and with all 1,000?
   Why so much longer than the third of a second that Shoal was frozen?

5. Send `FLUSHALL ASYNC` to a Shoal that holds a million keys, and right
   away, many `SET`s, with `valkey-benchmark`. Is the background
   destruction slowed down by the `SET`s, or the `SET`s by the
   destruction? (Hint: both run on the same shard, and
   [chapter](#ch:how-seastar-runs-your-code)'s task quota decides how they
   share it.) What happens to memory if a client sends `FLUSHALL ASYNC`
   again before the previous one has finished?
