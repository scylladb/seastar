# Designing a sharded application {#ch:designing-a-sharded-application}

Until now, however many shards Shoal had, it kept all its data, and all
its clients, on shard 0. This chapter makes Shoal use every core, with
the tools of
[chapter](#ch:talking-between-shards): messages between shards, and
services with an instance on every shard. The tools are the easy part.
The hard part is deciding where everything lives: which shard owns each
key, which serves each client, how a request reaches the data it needs,
and what to do about the commands, and the guarantees, that don't divide
neatly. Shoal version 8 answers these questions one at a time, and the
chapter ends by measuring whether the answers paid off.[^shoal-v8]

[^shoal-v8]: The source of this version is in `shoal/v8/`.

Version 8 has three sharded services, each with an instance on every
shard: `store`, which holds the data, `pubsub`, which keeps the Pub/Sub
subscriptions, and `server`, which accepts connections and serves them.
`shoal_main()` starts them, runs Shoal until it's asked to stop, and
then stops them:

```{.cpp include="shoal/v8/main.cc" snippet="shoal-main"}
```

## Dividing the data

Every key belongs to one shard, which keeps it, and executes every
command on it. Dividing data this way, among shards or among servers, is
called [*partitioning*]{.index .main}. Which shard is decided by a hash
of the key, as in [chapter](#ch:talking-between-shards)'s word counter:

```{.cpp include="shoal/v8/commands.cc" snippet="owner-def"}
```

A hash function spreads keys evenly over the shards, and every shard can
compute any key's owner on its own, without asking anyone. That's why
hashing is the usual way to divide data: ScyllaDB divides its rows among
shards by a hash of their partition key, and Redis Cluster divides keys
among servers by a hash, into 16,384 "slots". Its price is that keys
that belong together, the fields of one user, say, are scattered too.
Redis Cluster's answer is the *hash tag*: only the part of the key in
braces is hashed, so `{user1}.name` and `{user1}.email` always land
together. Shoal does the same, and the reason will become clear when we
get to commands on many keys.

The store's instance on each shard holds that shard's keys. It keeps
what version 7's store kept, the keys, their expiration times, the
clients blocked on them, and what stopping needs. It adds two things:
`waiting` is for a `BLPOP` on keys of several shards, which "Waiting on
several shards", below, explains. And `stop()`, which
`sharded<store>::stop()` calls, waits for the store's background work,
which version 7 waited for in `main.cc`:

```{.cpp include="shoal/v8/commands.hh" snippet="store"}
```

Shoal's two limits on memory become limits per shard: the limit on the
memory of the requests being read, and the limit on the memory of unsent
Pub/Sub messages. Each shard has its own memory, a share of the
machine's, as [chapter](#ch:shared-nothing) showed, so a limit per shard
follows the memory. Each is a semaphore, in a sharded service: the first
in `server`, and the second in `pubsub`. The limit on clients can't be
divided that way: `--maxclients` is a limit for the whole server, as in
Redis, so it must be counted in one place. "Limits and stopping", below,
shows how.

## Connections on every shard {index="load balancing!connections"}

Version 7 accepted every connection on shard 0, with the listener's load
balancing algorithm set to `fixed`. Version 8 runs an accept loop on
every shard, each with a listener of its own, on the same port, all with
the default algorithm, which gives each new connection to the shard that
has the fewest open connections. ([Chapter](#ch:networking) explains
the other algorithms.)

The accept loop is a method of `server`, whose instance on each shard
serves that shard's connections:

```{.cpp include="shoal/v8/main.cc" snippet="server"}
```

`server` has no `start()` method: `sharded<server>::start()`, in
`shoal_main()`, only constructs an instance on every shard. Accepting
begins when `run()`, the function that `shoal_main()` calls to run
Shoal (see "Limits and stopping", below), calls `accept_connections()`
on every shard, with `invoke_on_all()`:

```{.cpp include="shoal/v8/main.cc" snippet="accept"}
```

Each shard's loop stops as version 7's did, as
[chapter](#ch:clean-shutdown) explained: a stop makes `abort_accept()`
fail the `accept()` that the loop waits for, so the loop checks for a
stop after `accept()` returns, and then ignores what it returned.

Every shard calls `listen()` on the same port. The operating system
normally refuses that: a second socket can't listen on a port that
another socket already listens on. But Seastar's `listen()` isn't the
operating system's. The shards' listeners on one port share it, and
Seastar gives each new connection to the `accept()` of one of them, the
shard that the load balancing algorithm chose.
([Chapter](#ch:networking) shows how the connections get there.)

Once connections and keys are both divided, a request's key belongs to
the shard of its connection only by chance: with $n$ shards, a key of a
random request is on another shard $(n-1)/n$ of the time, half the
requests with two shards, three quarters with four. Those requests need a
hop, there and back. [Chapter](#ch:talking-between-shards)'s cost of a
hop, about a microsecond for a single request, and 45 nanoseconds each
when many are in flight, is now part of almost every request.

There is a way to avoid most of the hops: let the client choose the
shard. ScyllaDB's client drivers are *shard-aware*: they learn how
ScyllaDB divides the data, open a connection to every shard, and send
each request on the connection of the shard that owns its data.
[Chapter](#ch:networking) shows how a client can choose the shard of its
connection. But Redis clients know nothing about Shoal's shards, so
Shoal must forward.

## Forwarding a request {index="request forwarding"}

The connection's shard reads a request, knows which shard owns the keys
that the request names, and asks that shard to execute it. For this,
the command table now says where each command's keys are:

```{.cpp include="shoal/v8/commands.cc" snippet="table"}
```

As in every version since [chapter](#ch:coroutines)'s, the functions in
the table, such as `get()` and `set()`, execute the commands. But one
name changed its meaning: the function that version 7 called `run`,
which executes a command on a `store` and returns its reply, is now
`on_shard`, and `run` is the new kind that the comment describes.

`batch_shard()` uses the table to find the shard that owns all of a
request's keys, if one does, and `execute_here()` executes the command
there. The next section shows who calls them: the connection's shard
sends the request to that shard with `invoke_on()`, together with its
other requests to the same shard, in a batch, hence the name.

```{.cpp include="shoal/v8/commands.cc" snippet="batch"}
```

The reply that `execute_here()` returns, an `sstring`, travels back to
the connection's shard with the answer to the message. (Its memory is
allocated on the key's shard, and freed on the connection's. As
[chapter](#ch:shared-nothing) said, that works, but freeing memory of
another shard costs more than freeing the shard's own, for reasons that
[chapter](#ch:memory) explains.)

A request for which `batch_shard()` returns nothing is executed instead
by `execute()`, on the connection's shard, which calls the command's
`run`: it takes the whole `sharded<store>`, so the command can reach
other shards itself, and it returns a future, since the command may
wait.

## Many requests at once {index="pipelining"}

A client that pipelines sends many requests before it reads any reply,
and version 7 executed them one after another: each was done in a few
hundred nanoseconds, and waiting for the previous one cost nothing. Now
most of them wait for a hop, and executing them one at a time would make
a pipelined connection wait for a round trip per request, losing exactly
what pipelining is for. So version 8 executes a connection's requests at
the same time, and writes their replies in order. It took us three
attempts to get this right, and the two that failed are worth seeing.

The replies are the easy part. They wait in a queue, in the order of
their requests, and one coroutine, the connection's writer, writes each
in turn, as soon as it's ready:

```{.cpp include="shoal/v8/main.cc" snippet="ordered-replies"}
```

`reserve()` limits the requests of one connection in progress at once to
128, so that a client that pipelines without end makes Shoal stop
reading from it, rather than fill its memory. Each request also keeps
the units of the memory it was read with until its reply is written.
`idle()` tells the connection's loop, below, whether every reply so far
has been written. After a failed reply, it never says so again, so that
the client can't take a later reply for the failed one's.

[Chapter](#ch:concurrency-within-a-shard) wrote Pub/Sub's messages with
a chain instead, a coroutine for each message, which waited for the
previous one's write. One writer for all the replies is cheaper, and
made two shards about 4% faster than such a chain.

The first attempt sent each request to its shard as soon as it was read.
The replies came in the right order, and every test passed, until we ran
the tests against a `debug` build of Seastar. Then `test_pipeline_order`,
which sends a pipeline of `SET`s and `GET`s of the same keys, failed: a
`GET` returned nothing, for a key that the `SET` before it in the
pipeline had just set. The two requests had gone to the same shard, in
order, and we had assumed that the shard would execute them in that
order. In a `release` build, it does, because tasks run in the order in
which they were scheduled. But Seastar doesn't promise that, and a
`debug` build shuffles its tasks to catch code that relies on it, as
ours did.

The second attempt made each request wait for the connection's previous
request on the same shard to finish. It was correct, but it brought back
the round trip per request: a pipeline of 300,000 requests from one
connection took 2.5 seconds with two shards, against 0.94 with version 7.

The third attempt is [chapter](#ch:talking-between-shards)'s lesson:
batches. Each connection has a queue of requests for each shard. The
requests that the connection reads together are sent together, as one
batch, which the shard executes in order, one after the other, in one
task. Those that come while it's being executed wait, and are the next
batch:

```{.cpp include="shoal/v8/main.cc" snippet="shard-queue"}
```

A connection's requests to a shard are executed in order, and with them,
its requests to each key. And the busier a shard is, the larger the
batches it gets, and the fewer messages per request. The shard reads
the batch's requests from the connection shard's memory, through a
reference, rather than copying them: `batch` lives in `send()`'s frame,
unchanged, until the replies come back, which is the condition that
[chapter](#ch:talking-between-shards) gave for such a reference.

The `yield()` is what gathers the requests read together. A client that
pipelines sends many requests at once, and the connection's loop, below,
reads them from its buffer one after another, without ever waiting,
since the data is already there: a `co_await` on a future that is
already ready doesn't suspend the coroutine. Without the `yield()`, the
first of them was sent alone, as soon as it was read, and the rest
followed, as a second batch, after its reply. With it, `send()` waits
until the loop has read everything in the buffer, and sends it all in
one message. This made two and three shards 1% to 2% faster. The same
pipeline of 300,000 requests now took 0.95 seconds with two shards,
about as long as with version 7. The connection's loop decides how each
request is executed:

```{.cpp include="shoal/v8/main.cc" snippet="serve"}
```

A request whose keys are all on another shard goes to that shard's
queue, and the loop reads the next request at once. Anything else, a
command on keys of several shards, a command that may wait or yield,
such as `BLPOP` or `KEYS`, or a Pub/Sub command, is executed by
`execute()` on the connection's shard, by itself. The loop first waits
for the replies of the requests before it, so that the command sees
their changes and its reply comes after theirs, and reads the next
request only after the command is done, so that no later request
overtakes it. So such a request stops the connection's pipeline: its own
hops to other shards overlap with no other request's. The loop also
makes sure, however it ends, that every request it started has finished
before the connection's stream is closed: the requests hold references
to it.

One branch is there for speed alone: a request for the connection's own
shard is executed at once, without a queue, and its reply is written at
once if no earlier reply is waiting. The loop's first version didn't
have it. Every request went through its shard's queue, even on the
connection's own shard, and so with one shard, every request did.
Version 8 on one shard was then a third slower than version 7, with a
single key, and a profile, made with `perf record` as
[chapter](#ch:debugging-and-profiling) explains, found about 40% of its
time in the queues, their promises, and the chain of replies. The
shortcut takes the queue away from every request on the connection's own
shard: such a request never waits, so the connection's requests to its
own shard are executed in the order in which they were read, without
one. With it, and a few smaller savings, such as `owner()` not hashing
the key at all when there is one shard, version 8 on one shard is as
fast as version 7, as the measurements below show.

The order that version 8 keeps is the order of each shard's requests,
and so of each key's. It doesn't keep the order between shards: a client
that pipelines `SET a 1` and then `SET b 2`, with the two keys on
different shards, may have another client see `b` already set and `a` not
yet. Redis executes a pipeline in order, and never shows that. Redis
Cluster, whose keys are on different servers, doesn't promise it either,
and neither does Shoal.

## Commands on many keys {index="fan-out"}

`DEL` can take many keys, as in `DEL k1 k2 k3`, and they may be on
several shards. So can `EXISTS`. This section also adds to Shoal two
new commands on many keys: `MGET`, which gets the values of several
keys, and `MSET`, which sets them. When all of a command's keys
are on one shard, `batch_shard()` sends it there, like a command on one
key. Otherwise, the connection's shard *fans it out*: it sends each
shard that owns some of the keys a list of where those keys are in the
request, and combines the shards' answers:

```{.cpp include="shoal/v8/commands.cc" snippet="by-shard"}
```

```{.cpp include="shoal/v8/commands.cc" snippet="del-exists"}
```

`EXISTS` is the same, with a function that counts the keys rather than
deleting them. `MGET` must return the values in the order of the keys,
so each shard returns its keys' values, and the connection's shard puts
each in its place:

```{.cpp include="shoal/v8/commands.cc" snippet="mget-mset"}
```

`MSET` is `DEL` again: each shard sets its own keys, and the connection's
shard waits for all of them.

Fanning out makes the commands work, but changes what they promise.
Redis's `MSET` is atomic: no client ever sees some of its keys set and
others not yet. Version 8's `MSET` is atomic only for each shard's keys:
the shards set theirs independently, at slightly different times. And if
one shard fails, others may already have set their keys, and the client
gets an error for a command that partly happened. Making a command on
several shards atomic needs more machinery, locks, or a protocol such as
two-phase commit, which costs at least one more round of messages, and
makes every shard wait for the slowest. Redis Cluster refuses instead:
a command on keys of different slots fails with a `CROSSSLOT` error,
and applications use hash tags to keep the keys of one `MSET` together.
Shoal accepts such commands, and with hash tags, they are atomic, since
they go to one shard, in one batch.

Some commands concern every shard. `KEYS` asks each shard for its
matching keys, with [`map()`]{index="`seastar::sharded`!`map()`"}, and
`DBSIZE`, new in version 8, adds up the shards' counts, with
`map_reduce0()`:

```{.cpp include="shoal/v8/commands.cc" snippet="keys"}
```

`keys_here()` goes over one shard's keys a bucket at a time, as
[chapter](#ch:how-seastar-runs-your-code)'s `KEYS` did. And `FLUSHALL`
runs on every shard with `invoke_on_all()`.

## Pub/Sub on every shard {index="Pub/Sub!across shards"}

A subscriber is served by its connection's shard, but a publisher may be
on any shard, so a message must reach every shard. Pub/Sub becomes a
sharded service: each shard's instance keeps the channels of that
shard's subscribers, so `SUBSCRIBE` and `UNSUBSCRIBE` don't change.
`PUBLISH` asks every shard, with
[`map_reduce0()`]{index="`seastar::sharded`!`map_reduce0()`"}, to send
the message to its own subscribers, and adds up how many each one sent
it to. What a shard does, in `publish_here()`, is what version 7's
`PUBLISH` did, so its body is left out here:

```{.cpp include="shoal/v8/pubsub.cc" snippet="publish"}
```

Each shard keeps its own limit on its subscribers' unsent messages, and a
publisher waits, on each shard, for room there. The cost of this design
is that every `PUBLISH` visits every shard, even if the channel's
subscribers are all on one shard, or there are none. A server with many
shards, or many messages, would rather keep, for each channel, the set
of shards that have subscribers, and visit only those, as exercise 2 does.

## Waiting on several shards {index="BLPOP!across shards"}

`BLPOP` on one key is a command on one key: its shard executes it, and
waits there. `BLPOP` on keys of several shards is the hardest command to
divide. The client waits for the first push to any of its keys, wherever
it is, and must get exactly one element: two shards that push at the
same moment must not both pop an element for it, since one of the
elements would be lost. So every shard needs to know whether the wait is
already over, and only one can end it.

The client's shard keeps the wait, and every shard that owns one of its
keys keeps the client's place in the line of clients waiting for that
key, with a pointer to the wait:

```{.cpp include="shoal/v8/commands.hh" snippet="blpop-wait"}
```

`done` is a `std::atomic<bool>`, the one piece of memory in Shoal that
several shards may change at the same moment. Whoever ends the wait, a
shard that pops an element for it, or the client's shard, when the
timeout expires or the client hangs up, first changes `done` from
`false` to `true`, with `exchange()`, and only the one that found it
`false` may go on. [Chapter](#ch:shared-nothing) warned that sharing
makes the shards slow, when they touch the same memory often. This flag
is touched a few times in a wait's life, by the few shards that the
client's keys belong to, which is too rarely to matter. And the
alternative, a round of messages to agree on who wins, would be slower,
and harder to get right.

The client's shard visits the keys in order, popping an element from the
first that has one, or joining the line of each:

```{.cpp include="shoal/v8/commands.cc" snippet="blpop"}
```

A push to a key serves the clients in the key's line, in order, skipping
those whose wait has already ended elsewhere, and sends the element to
the client's shard ([Figure](#fig:blpop-shards)):

![A client on shard 0 waits in `BLPOP` on two keys, one on shard 1 and
one on shard 2. Pushes to both keys come at the same time: shard 2
changes `done` first, pops its element, and sends it to shard 0. Shard
1 finds `done` already set, and leaves its element in the list. Shard 0
then has both shards forget the wait, and replies.](figures/blpop-shards.svg){#fig:blpop-shards placement="tb"}

```{.cpp include="shoal/v8/commands.cc" snippet="serve-blocked"}
```

The wait lives in the frame of `blpop()`, on the client's shard, and the
other shards have pointers to it, so before `blpop()` returns, it asks
every shard it visited to forget the wait, and waits for all of them:
after that, no shard can use the pointer. That's
[chapter](#ch:object-lifetime)'s rule, across shards: an object that
others use must live until all of them are done with it.

## Limits and stopping

`--maxclients` is a limit for the whole server, so it's counted in one
place, on shard 0. Giving each shard a share of it instead would make a
shard whose share is used up refuse a client while other shards still
had room, whenever the connections aren't spread evenly, as when clients
choose their shards. Counting on one shard is the "only on shard 0"
design of [chapter](#ch:talking-between-shards): one shard keeps the
state, and the others ask it. A shard that accepts a
connection asks shard 0 to admit it, by calling `admit_client()` on
shard 0's instance of `server`. It reaches that instance with
`container()`, which is why `server` derives from
`peering_sharded_service`. This costs a hop per connection, cheap next
to everything else that a connection costs:

```{.cpp include="shoal/v8/main.cc" snippet="handle-connection"}
```

Versions 6 and 7 held a `semaphore_units` for each connection, given
back when it ended. That doesn't work across shards, since units must be
given back on their semaphore's shard. So shard 0 keeps a plain count,
and `defer()` asks shard 0 to decrease it when `handle_connection()`
ends, whether it returns or throws. Nothing waits for that message, but
it carries the connection's hold on the gate, so Shoal can't finish
stopping, and destroy shard 0's `server`, before the count is decreased.

The rest of `handle_connection()` serves the connection, as version 7's
did.

The comment about subscribing before the first `co_await` is there
because we got it wrong at first. Version 7 subscribed to `stopping`
right after accepting, before anything waited. Version 8's first draft
asked shard 0 to admit the connection first. A stop that came while the
connection waited for shard 0 was missed: the later `subscribe()`
returned an empty subscription, without calling the function, which is
what exercise 3 of [chapter](#ch:clean-shutdown) asks about, and a
client that sent nothing more kept Shoal from stopping.

Stopping is now a matter for every shard. Signals arrive at shard 0, as
`SHUTDOWN` does, from whichever shard executes it. Shard 0 then requests
`stopping` on the others, and waits for every shard's accept loop to end
and its connections to finish, with a grace period after which every
shard gives up on its connections:

```{.cpp include="shoal/v8/main.cc" snippet="run"}
```

`shoal_main()`, at the start of this chapter, then stops the three
services.

## Did it pay off? {index="Shoal!version 8, performance"}

We measured version 8 against Valkey, and against version 7, the version
of [chapter](#ch:clean-shutdown), which kept all its data and clients on
shard 0, and so used one core. We used `valkey-benchmark`, as in
[chapter](#ch:coroutines): 40 million `SET`s, and then `GET`s for half
a minute. The `SET`s are a fixed number, not a fixed time: with random keys, their
number decides how many of them add keys and how many replace values,
and how full the store is for the `GET`s, which change nothing. The
requests were sent in pipelines of 64, on 64 connections, with the
server and the client on separate cores. The
laptop has four fast cores, which the server and the client had to
share, so the client ran on the remaining fast cores, and for the larger
servers, on the four slow cores as well. With a server on all four fast
cores, the client had only the slow ones. A client with fewer threads,
on fewer cores, gave the same results, which shows that the server was
the limit. We ran it with two kinds of keys: random keys, out of ten
million (`-r 10000000`), and a single key, for every request (no `-r`).
On several shards, 40
million `SET`s take only seconds, so we timed them with a clock of our own,
over several `valkey-benchmark` processes without `--threads`, rather
than in its quarter-second steps ([chapter](#ch:tutorial-introduction)).
Version 8 ran one shard on each core. Valkey ran on one core, and on
more with `--io-threads`, which makes it read and write its clients'
sockets on several threads, one for each core. The best of three runs,
in millions of requests a second:

| Server | Cores | `SET`, random keys | `GET`, random keys | `SET`, one key | `GET`, one key |
|---|---|---|---|---|---|
| Shoal 7 | 1 | 1.29 | 1.27 | 3.39 | 3.78 |
| Shoal 8 | 1 | 1.26 | 1.28 | 3.35 | 3.75 |
| Shoal 8 | 2 | 2.23 | 2.28 | 5.26 | 5.79 |
| Shoal 8 | 3 | 3.10 | 3.38 | 6.79 | 7.21 |
| Shoal 8 | 4 | 4.15 | 4.36 | 8.51 | 8.45 |
| Valkey | 1 | 1.96 | 2.98 | 2.55 | 3.31 |
| Valkey | 2 | 2.49 | 4.27 | 3.59 | 5.25 |
| Valkey | 3 | 2.34 | 3.86 | 3.21 | 4.50 |
| Valkey | 4 | 2.24 | 3.61 | 3.05 | 4.23 |

The rest of this section is about random keys. The single key is a case
of its own, which "Hot spots", below, comes back to.

On one shard, version 8 is as fast as version 7, which is what the
shortcut for requests on the connection's own shard, in "Many requests
at once", is for. Two shards do 1.8 times as much as one, and three
shards 2.6 times as much (for `GET`s). That's less than two and three
times, and for a reason that we can measure. With two shards, half of
every connection's requests are on the other shard, and with three, two
thirds: they are forwarded, there and back. The hop itself is cheap in
a batch, a few dozen nanoseconds per request, as
[chapter](#ch:talking-between-shards) measured, but a forwarded request
costs more than its hop. It waits in a queue, gets a promise for its
reply, which comes back in a message, and its reply is written by the
writer rather than at once. A `perf` profile of two shards found the
extra work spread thin, over many small costs like these, none of them
large.

Suppose that a forwarded request costs a fraction $f$ more than a
request executed on its connection's shard. On $n$ shards, $(n-1)/n$
of the requests are forwarded, so a request costs, on average,
$1 + f\,(n-1)/n$ times as much as on one shard, and if one shard
does $T_1$ requests a second, $n$ shards do
[$$T_n = \frac{n\,T_1}{1 + f\,\frac{n-1}{n}}.$$]{text="Tₙ = n T₁ / (1 + f (n − 1)/n)."}

The two-shard measurement gives $f = 0.25$: a forwarded request costs
about a quarter more. With that, the formula predicts 3.30 million
`GET`s a second on three shards, and we measured 3.38. On four shards,
the formula predicts 4.32 million `GET`s a second, and we measured
4.36.

![Shoal version 8's `GET`s a second, measured on one to four shards,
and projected to more with the formula, with $f = 0.25$. The projection
is a straight line: each shard adds the same, 80% of what one shard does
alone.](figures/sharded-scaling.svg){#fig:sharded-scaling}

The formula lets us look past the laptop's four shards
([Figure](#fig:sharded-scaling)). As $n$ grows, $(n-1)/n$ approaches
1, and $T_n$ approaches a straight line, $n\,T_1/(1+f)$:
every shard adds the same, $1/1.25$, or 80%, of what one shard does
alone. That's linear scaling, which is what Seastar promises. It's just
not a multiple of one shard's speed, because one shard alone forwards
nothing. The first few shards hide this. The step from one shard to two
starts forwarding half of the requests at once, while the steps after it
add fewer and fewer: a sixth more on the third shard, a twelfth on the
fourth. So scaling looks worse on two and three shards than it is on
many: on 16 shards, the formula predicts 16.6 million `GET`s a second,
13 times one shard's speed.

The projection assumes that a forwarded request costs the same, however
many shards there are. That holds while the batches stay large: with
dozens of shards, a connection's pipeline of 64 requests has only a few
for each shard, and needs more messages per request, so a server that
large wants clients that pipeline more. And a larger machine has limits
that this laptop can't show, such as its memory's bandwidth and its
network card's. The formula is a reason to expect Shoal to scale, not a
measurement of it.

`SET` scales a little less than `GET`, 2.5 times on three shards. The
difference is in adding keys: a second pass of `SET`s over the same ten
million keys, which only replaces their values, scaled about as well as
`GET`, 2.7 times. While keys are added, each shard's hash table grows,
and every time it grows, it moves all its keys to a new array of
buckets, in one call that can't yield, as
[chapter](#ch:how-seastar-runs-your-code) explained: the shard stalls.
And the other shards wait too, we think, because every connection has
requests on every shard, and `valkey-benchmark` sends a connection's
next pipeline only after all the replies to the last one. (With idle
polling turned off, so that a shard's CPU time measures its work, the
shards were busy 89% of the time while adding keys, against 93% during
the `GET`s.)

And Valkey? With random keys, one Valkey thread is far faster than one
shard of version 8, for `GET`s: 2.98 million a second against 1.28
million. With a single key, it's slower than version 8 on one shard. The
same holds on two cores: with a single key, version 8 on two shards is
faster than Valkey on two cores, for `SET`s and for `GET`s, but with ten
million keys, Valkey does almost twice as many `GET`s.

So Shoal's handling of requests isn't slow: its lookups among ten
million keys are. One shard of version 8 does almost three times as many
`GET`s of one key as of ten million. A single key stays in the
processor's caches, but ten million keys don't fit there, so a lookup
in `std::unordered_map` must load from main memory: the key's bucket,
then the node it points to, then the key's characters, then the value.
Each load needs the address that the one before found, so they happen
one after another, and each takes as long as the processor needs to
execute about 1,700 instructions. [Chapter](#ch:memory) measures these
loads, and replaces `std::unordered_map` with a hash table laid out for
the processor's caches, which makes Shoal faster, and closes the gap to
Valkey.

Valkey's threads read and write its clients' sockets, but every command is still
executed by its one main thread, and more threads didn't help it: with
three and four, it did fewer requests than with two. (We didn't look
into why.) Shoal's way, each shard executing its own keys' commands, is
what scales: on four cores, version 8 did as many `GET`s of ten million
keys as Valkey at its best, and 1.6 times as many `GET`s of one key.

## Hot spots {index="hot spot"}

Hashing divides the keys evenly, but not the requests: if most requests
are for one key, its shard gets most of the work, and the others wait
for it. Such a key is a *hot spot*. The benchmark with a single key, in
"Did it pay off?", sends every request to one key. Surprisingly, it
still scaled: version 8 did 3.35, 5.26, 6.79 and 8.51 million `SET`s a
second on one, two, three and four shards. A `SET` of one value is
cheap, and most of a request's cost, reading it, parsing it, and writing
the reply, is on the connection's shard, which is divided among the
shards anyway. Only the cheap part goes to the key's shard.

A hot spot that hurts takes a key whose requests are expensive. In
`valkey-benchmark`'s `lrange_100` test, every request reads the first
100 elements of one list. The best of three runs:

| Server | `LRANGE` of 100 elements, a second |
|---|---|
| Shoal version 7 | 213,000 |
| Shoal version 8, 1 shard | 232,000 |
| Shoal version 8, 2 shards | 258,000 |
| Shoal version 8, 3 shards | 260,000 |

Each request makes a reply of 100 elements, on the list's shard, and
that shard sets the pace: a second shard helps a little, by taking over
half of the connections' reading and writing, and a third doesn't help
at all. No way of dividing keys among shards can divide the requests of
one key, so a hot spot is fixed in the application, or in the data: by
splitting a hot key into several, by caching its value where it's read,
or, for data that is read far more than written, by keeping a copy on
every shard. ScyllaDB has the same problem with a partition that is
much more popular than others, and the same advice for its users.

The slowest shard sets the pace in another way, too, on machines whose
cores aren't equal, such as the laptop that these measurements were made
on, with its fast and slow cores. Exercise 4 shows how much.

## Exercises

1. Add Redis's `SCAN cursor`, which returns some of the keys and a
   cursor for the next call, until the cursor is 0 again, so that a
   client can list all the keys without stopping the server for long.
   How can a cursor say where to continue, when the keys are on several
   shards? (Hint: a cursor is a number, and part of it can be a shard's
   number.) Write its tests first, and check them against Valkey. What
   may they assume about the keys returned, when keys are added and
   deleted between calls?

2. Make `PUBLISH` visit only the shards that have subscribers to its
   channel. Where would you keep, for each channel, the set of those
   shards: on shard 0, or a copy on every shard? Which costs `PUBLISH`
   less, and which costs `SUBSCRIBE` less? Measure how long a `PUBLISH`
   to a channel with one subscriber takes, on four shards, before and
   after, with many `PUBLISH`es in a pipeline. And make sure that no
   message is lost: a `PUBLISH` that comes after a client got its
   `SUBSCRIBE`'s confirmation must reach it.

3. In `blpop()`, remove the loop that calls `forget_wait()` on every
   shard. Which test fails against a `debug` build, and what does
   AddressSanitizer report? Why did the `release` build pass?

4. Shards don't always run on equal cores: some machines, like the
   laptop that this book's measurements were made on, mix fast cores
   with slower ones. Run version 8 on two shards, one on a fast core and
   one on a slow one (`--cpuset 0,4` on that laptop), and measure it as
   "Did it pay off?" did, with random keys. Compare it with two fast cores,
   and with one. Before you run it, predict the result: is a slow shard
   worth having?
