# Seastar RPC {#ch:rpc}

The servers of a cluster talk to each other all the time. In ScyllaDB, a
node that receives a client's write sends it to the nodes that hold
copies of the data, and waits for their answers; reads, the gossip by
which nodes learn of each other, repairs, and the streaming of data to a
new node all travel between nodes too. All of this goes over Seastar's
*RPC*, from `<seastar/rpc/rpc.hh>`, a framework for remote procedure
calls: one server calls a function, a *verb*, on another, with
arguments, and gets back its result, or its exception. ScyllaDB's
`messaging_service`, which carries all of the traffic between its nodes,
is built on it. Redpanda, which also needs its brokers to talk to each
other, wrote its own RPC framework instead, with a header that carries
checksums of the message, and C++ code generated from a JSON description
of each service; Seastar's RPC isn't the only way, but it is the one that
comes with Seastar.

Shoal's version 14 uses it for *replication*: a replica, another Shoal,
copies a primary's keys, and then applies its changes as they happen, as
a Redis replica does.

## Verbs and a serializer {index="RPC"}

A protocol is a
[`rpc::protocol<Serializer, MsgType>`]{index="`seastar::rpc::protocol`" .main}:
`MsgType` numbers its verbs, and `Serializer` is a type whose only use is
to select the functions that write a value into a message and read it
back. RPC serializes each argument, and each result, with an overload of
`write()` and `read()` whose first parameter is the serializer, and it
knows of no types by itself, not even `int`. The example program
`examples/rpc/verbs.cc` writes integers in little-endian order, and a
string as its length and its bytes:

```{.cpp include="examples/rpc/verbs.cc" snippet="serializer"}
```

Each verb is a number, which goes in every message, and a server's
handler for it is a function that
[`register_handler()`]{index="`seastar::rpc::protocol::register_handler()`" .main}
attaches to the number. The handler's parameters are the verb's
arguments, and what it returns, a value or a future of one, is the
result. A handler may also take, before the arguments, a
[`const rpc::client_info&`]{index="`seastar::rpc::client_info`"}, which
tells it the caller's address. This type, and the others of this
chapter, other than the compressors, are in `<seastar/rpc/rpc_types.hh>`,
which `<seastar/rpc/rpc.hh>` includes:

```{.cpp include="examples/rpc/verbs.cc" snippet="verbs"}
```

The server, a `protocol::server`, listens on an address, and a client, a
`protocol::client`, is a connection to a server, which it opens as soon
as it's created:

```{.cpp include="examples/rpc/verbs.cc" snippet="start"}
```

The client side of a verb is a function that
[`make_client()`]{index="`seastar::rpc::protocol::make_client()`" .main}
makes from the verb's signature: given a client and the arguments, it
sends the message, and returns a future of the result. The signature
must be one that the server's handler accepts, but RPC can't check it:
only the numbers and the bytes go over the network. The program calls
each verb:

```{.cpp include="examples/rpc/verbs.cc" snippet="calls"}
```

```
1 + 2 + 3 = 6
an older client's 1 + 2 = 3
note from 127.0.0.1: hello
fail: remote_verb_error: std::runtime_error (no key 'x')
wait: timeout_error: rpc call timed out
stats: unknown_verb_error: verb 5
after the server stopped: closed_error: connection is closed
```

A client may have any number of calls in progress on its connection, and
their replies come back in the order in which they're ready, each matched
to its call by a number that the message carries. On the server, the
connection's loop reads a message, waits for memory for it (see below),
and starts its handler, without waiting for the handler to finish before
it reads the next message. Handlers start in the order of the messages,
but once one of them waits, the next ones run: two calls on one
connection may change things in either order. Shoal's replication,
below, depends on its changes being applied in order, so it never has two
calls in progress at once.

## When a call fails {index="RPC!errors"}

What a client gets when a call doesn't succeed tells it what happened:

- **The handler threw.** The server sends the exception back, but only as
  text, and the client's future fails with
  [`rpc::remote_verb_error`]{index="`seastar::rpc::remote_verb_error`"},
  whose message is the exception's type and message: a client can't
  catch the server's exception by its type. A result that the caller
  should act on, such as "no such key", belongs in the reply, not in an
  exception.
- **The reply took too long.** A call may be given a timeout, as a
  duration or a time point, before its arguments, and the client stops
  waiting at that time, with
  [`rpc::timeout_error`]{index="`seastar::rpc::timeout_error`"}. The
  server doesn't stop the handler. It does learn the caller's deadline:
  the client sends the time left, in milliseconds, so the clocks of the
  two machines needn't agree, and a handler whose first parameter (after
  `client_info`, if it has that) is an
  [`rpc::opt_time_point`]{index="`seastar::rpc::opt_time_point`"} gets
  it. A request whose deadline passes while it waits for memory on the
  server is dropped, without a reply, and a reply that is ready only
  after the deadline isn't sent at all.
- **The call was canceled.** A call given an
  [`rpc::cancellable`]{index="`seastar::rpc::cancellable`"} can be
  canceled with its `cancel()`, and then fails with
  [`rpc::canceled_error`]{index="`seastar::rpc::canceled_error`"}.
- **The server has no such verb.**
  [`rpc::unknown_verb_error`]{index="`seastar::rpc::unknown_verb_error`"},
  with the verb's number.
- **The connection failed.** Every call in progress on it fails with
  [`rpc::closed_error`]{index="`seastar::rpc::closed_error`"}, and so
  does every later call on that client: a client is one connection, and
  doesn't reconnect. The caller must stop it, and make a new one.

A connection to a server that doesn't answer at all fails only when the
kernel gives up on it, after about two minutes by default, and until
then the client's calls wait in its queue: calls to a server that may
be down should have a deadline. The client's constructor also takes an
[`rpc::client_options`]{index="`seastar::rpc::client_options`"}, whose
`keepalive`, off by default, makes the kernel notice a server that
vanished without closing the connection, and whose `tcp_nodelay`, on by
default, sends small messages at once.

A client and a server are both stopped with `stop()`, once. A server's
second `stop()` fails an assertion. A client's asks its promise for a
future a second time, which only a debug build's assertion catches; a
release build goes on, with undefined results. The first version of
the example program stopped its server twice, once to show the closed
connection, and again on the way out, and aborted.

A handler that returns
[`rpc::no_wait`]{index="`seastar::rpc::no_wait`"}, of type
`rpc::no_wait_type`, makes the verb *one-way*: the server sends no reply,
and the client, whose signature also returns `rpc::no_wait_type`, gets a
future that is ready once the message is sent, or failed to be: it never
fails, and if the handler fails, the client never knows. ScyllaDB sends a client's write to the other nodes this
way, and each answers, when its write is done, with a one-way message of
its own, back to the node that sent it. A reply could only go back over
the connection that the request came on, but ScyllaDB sends a write to
one node in each remote datacenter, which passes it on to the others
there, and they all answer the original node directly.

## Changing a protocol {index="RPC!compatibility"}

A cluster is upgraded one node at a time, so for a while its nodes run
different versions, and talk to each other. Seastar keeps its own part
of the protocol compatible ([chapter](#ch:upgrading-seastar)): when a
connection opens, its two ends tell each other which of RPC's optional
features they have, such as compression and sending the caller's
deadline, and use only those that both have. RPC lets a verb gain
arguments at the end, as
[`rpc::optional<T>`]{index="`seastar::rpc::optional`"}: a handler whose
last parameter is an `rpc::optional` gets nothing there from a client
that doesn't send it. The example's `add` gained a third number that way,
and the "older client", which sends two, still works. A handler ignores
arguments beyond those it takes, and a result can grow the same way: a
verb that returns an
[`rpc::tuple`]{index="`seastar::rpc::tuple`"} of several values may
append `rpc::optional` ones. A new verb, though, fails on an old server
with `unknown_verb_error`, so a new node must not use it until every
node has been upgraded. ScyllaDB describes its verbs in files from which
a script generates the code, and marks each argument that was added later
with the version that added it: its write verb has gained six
arguments so far, each an `rpc::optional` to its handler.

## Shoal replicates {index="replication"}

A Redis replica connects to its primary, receives a copy of all its
keys, and then every change that the primary makes, which it applies as
it receives them. Its clients may read its keys, but not change them.
Shoal 14 replicates the same way, over Seastar's RPC, with a few
differences: the replica connects to a port of its own on the primary,
`--replication-port`; it must have as many shards as the primary does,
so that each of its shards has the keys of the primary's shard of the
same number; and each of its shards replicates that shard alone, pulling
the shard's changes from the primary.

The changes are the requests that the shard's log already records
([chapter](#ch:files-and-storage)): `record()` gives each one to the
shard's *feed* too, which keeps the latest of them, in RESP, numbered by
their offset since Shoal started:

```{.cpp include="shoal/v14/feed.hh" snippet="feed"}
```

The feed keeps at least `--repl-backlog-size` bytes, divided among the
shards, 10 MB by default, as Valkey's backlog of the same name. Its
`copy()` drops the oldest chunk once the others hold enough, so a replica
that falls behind by more than that finds that its next change is gone.

The protocol has two verbs, and the serializer of the example program:

```{.cpp include="shoal/v14/replication.hh" snippet="verbs"}
```

A replica's shard calls `SYNC`, deletes its keys, and then calls `FETCH`,
again and again, applying what each returns. `SYNC` starts a *snapshot*
of the shard's keys, which writes them into the feed, as requests that
recreate them, after the changes it already holds. The snapshot works as
the log's rewrite does ([chapter](#ch:scheduling-groups)): while it
writes, every change goes on reaching the feed, and the replica applies
the snapshot's requests and the changes in the order of the feed, so
each key's snapshot replaces its earlier changes, and its later changes
come after. The snapshot's own requests must not push out changes that
the replica hasn't fetched yet, so it writes one key at a time, and
waits while half the feed is waiting to be fetched:

```{.cpp include="shoal/v14/commands.cc" snippet="snapshot"}
```

A replica's connection lands on whichever shard of the primary accepts
it, so the primary's handlers run each request on the shard that it's
about, with `invoke_on()`. `FETCH` takes the caller's deadline, and if
the feed has no new changes, it waits for some, but answers before the
caller would give up, with nothing if nothing came: a *long poll*, which
gets a change to the replica as soon as it's made, without the replica
asking again and again:

```{.cpp include="shoal/v14/replication.cc" snippet="handlers"}
```

The primary listens on every shard, as Shoal's own server does, and its
connections use the LZ4 compressor, of which more below. Stopping it
first ends the feed's waits, which would otherwise keep the server
waiting for them:

```{.cpp include="shoal/v14/replication.cc" snippet="listen"}
```

On the replica, `FETCH` is the source of an input stream: a
`data_source_impl` ([chapter](#ch:networking)), whose `get()` returns the
stream's next buffer, here the next changes that `FETCH` returns. The
stream gives them to Shoal's request reader, which parses them as it
parses a client's requests, and as it parses the log when Shoal starts:

```{.cpp include="shoal/v14/replication.cc" snippet="fetch-source"}
```

```{.cpp include="shoal/v14/replication.cc" snippet="sync-and-apply"}
```

The reader asks the stream for more only after every request it read
before was applied, so the offset of each `FETCH` is also how far the
replica has applied the changes. Each request is applied on the
replica's shard, as the log's replay applies it, except `FLUSHALL`: the
primary's shard recorded it when it deleted its own keys, and on the
replica it deletes only the shard's keys, not every shard's:

```{.cpp include="shoal/v14/commands.cc" snippet="apply"}
```

What the replica does when a call fails depends on why it failed, which
the exceptions of the previous section tell. A connection that failed,
or a call that took too long, which may mean that the primary is gone,
is a reason to connect again, and to call again, with the same offset,
which the primary's feed still has if it didn't fall too far behind:

```{.cpp include="shoal/v14/replication.cc" snippet="call"}
```

An exception from the primary's handler, a `remote_verb_error`, means
that the primary can't continue this replication: the feed no longer has
the offset, or it's a different feed, of a primary that restarted. Then
the replica starts again, with `SYNC`:

```{.cpp include="shoal/v14/replication.cc" snippet="run"}
```

The replica's clients get Valkey's error for any command that changes
keys, which the command table now marks:

```{.cpp include="shoal/v14/main.cc" snippet="read-only"}
```

```
$ shoal -c2 --port 6379 --replication-port 7000 &
$ shoal -c2 --port 6380 --dir replica-data --replicaof 127.0.0.1:7000 &
$ redis-cli -p 6379 RPUSH list a b c
(integer) 3
$ redis-cli -p 6380 LRANGE list 0 -1
1) "a"
2) "b"
3) "c"
$ redis-cli -p 6380 SET x 1
(error) READONLY You can't write against a read only replica.
```

Shoal 14's tests start a primary and a replica, and check that the
replica copies the keys that the primary had, applies its changes, and
refuses its own clients' changes; and that it catches up after it
restarts, after the primary restarts, and after it falls behind by more
than the backlog, which a test arranges by stopping the replica's
process, with `SIGSTOP`, while the primary changes 10,000 keys. They also
pass against Valkey, whose replica connects to the primary's usual port,
with its `--replicaof HOST PORT`.

The test that kills the primary, and starts it again on the same ports,
failed now and then at first: the test connected to the new primary
before it was even running, and then its connection was reset. In our runs,
a Seastar server killed while it used the io_uring backend
([chapter](#ch:how-seastar-runs-your-code)) sometimes left its listening
socket accepting connections for a moment after its process was gone,
and then the kernel reset them; with the epoll backend, this never
happened. (We didn't find out why.) Now the test waits until the old ports refuse connections
before it starts the new primary.

## Compression, memory and isolation {index="RPC!compression"}

Both ends of a connection may offer a compressor factory, in their
`client_options` and `server_options`, and when the connection opens,
they agree on one. Seastar has two LZ4 compressors:
[`lz4_compressor`]{index="`seastar::rpc::lz4_compressor`"}, from
`<seastar/rpc/lz4_compressor.hh>`, which needs each message in one
contiguous buffer, and so copies a large one together, and
[`lz4_fragmented_compressor`]{index="`seastar::rpc::lz4_fragmented_compressor`"},
from `<seastar/rpc/lz4_fragmented_compressor.hh>`, which compresses a
message in its fragments, as it is. A
[`multi_algo_compressor_factory`]{index="`seastar::rpc::multi_algo_compressor_factory`"},
from `<seastar/rpc/multi_algo_compressor_factory.hh>`, offers several, and the connection uses the first in the client's list
that the server has too. ScyllaDB offers a compressor of its own, based
on zstd, then both of Seastar's LZ4 compressors. Shoal's feed is text,
the RESP of its requests, which compresses well, and Shoal offers the
fragmented LZ4 compressor, which needn't copy a reply of up to 128 KB
together before it compresses it.

A server limits the memory of the requests that it's handling, with the
[`rpc::resource_limits`]{index="`seastar::rpc::resource_limits`"} given
to it: a request is admitted only when the memory it's estimated to use,
`basic_request_size` plus its size times `bloat_factor`, fits in
`max_memory`, and it holds that memory until its handler's reply is
sent. A client that sends faster than the server handles its requests
thus waits for the server, which reads no more until memory is freed.
ScyllaDB estimates 1000 bytes plus three times a request's size, and
lets requests use up to 8% of a shard's memory.

A connection's messages are read, and its handlers run, in one
scheduling group ([chapter](#ch:scheduling-groups)): by default, the
group in which the server was created. A client may send an
*isolation cookie*, a string in its `client_options`, which the server's
`resource_limits::isolate_connection` function turns into a scheduling
group for the connection. ScyllaDB has *service levels*, classes of work
that its users define, each with its own scheduling group, and its own
share of the CPU. It sends the reads and writes of each service level to
another node over connections of their own, whose cookie names the
service level, so the work that one user's requests cause on other nodes
gets that user's share there too.

A connection also delivers its messages in order, so a small message
waits behind a large one sent before it. ScyllaDB sends its verbs over
five connections to each node, by kind: gossip, and the other verbs that
must get through quickly, on one; streaming and repair on another;
reads and writes on a third; the one-way answers to writes on a fourth,
so they don't wait behind the writes; and requests for distributed
aggregation on a fifth.

## Streams {index="RPC!streams"}

For large transfers, such as the data that a new node receives, RPC has
*streams*: a client makes an
[`rpc::sink<T...>`]{index="`seastar::rpc::sink`"}, which opens a
connection of its own to the server, and passes it as an argument of a
verb, whose handler receives it as an
[`rpc::source<T...>`]{index="`seastar::rpc::source`"}. The client
writes messages to the sink, and the handler reads them, in order, from
the source; the handler may return a sink of its own, which the client
receives as a source, for messages in the other direction. ScyllaDB
streams data to new nodes this way, and the rows that repair compares.
A stream's two ends must be closed by the application: the sink by its
`close()`, without which its destructor fails an assertion, and aborts
the program, and the source by reading it until it ends, with an empty
optional, or fails. A stream's connection is closed only when both are,
even when the stream failed.

Shoal's replication could have been a stream from the primary's shard,
rather than calls. Calls made it simpler to resume where the replica
stopped: each `FETCH` names its offset, and any `FETCH` may be the first
on a new connection.

## Exercises

1. Add Redis's `WAIT numreplicas timeout`, which waits until the
   changes that the client made before are applied on at least
   `numreplicas` replicas, or the timeout passes, in milliseconds, and
   returns how many replicas applied them. Shoal has at most one replica,
   so the answer is 0 or 1. What does the primary know about what the
   replica applied?

2. A replica's `FETCH` lands on any shard of the primary, which then
   passes it to the right shard. Make each connection land on the right
   shard instead. Look at `server_options::load_balancing_algorithm`, and
   at the local address of the client's constructor.

3. Encrypt the replication protocol with TLS, using the certificate
   options that Shoal's own port uses ([chapter](#ch:networking)). Check
   that compression still works.

4. Why must a replica have as many shards as its primary? What would a
   replica with a different number of shards have to do differently, and
   which requests of the feed would be hard to apply?
