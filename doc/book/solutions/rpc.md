# Seastar RPC {#sol:rpc}

## Exercise 1

The primary knows how far the replica applied each shard's changes: the
offset of its latest `FETCH`, since the replica asks for more only after
it applied everything before. So `WAIT` takes each shard's end of the
feed, and waits on that shard until the replica fetches from there on.
The patch is `examples/solutions/shoal-wait.patch`:

```{.cpp include="examples/solutions/shoal-wait.patch" file="commands.cc" snippet="wait"}
```

The offset alone isn't enough, though. A replica that started its
snapshot after a change has the change only once it has applied the
snapshot, which comes later in the feed than the offset it started from.
The feed remembers where the latest snapshot ended, and a replica has
everything before an offset only when it has fetched beyond both:

```{.cpp include="examples/solutions/shoal-wait.patch" file="feed.cc" snippet="wait-for-replica"}
```

The patch's tests check that a key is on the replica as soon as `WAIT`
returns, after a change and after a snapshot of 10,000 keys; that `WAIT`
returns 0 at its timeout without a replica; and that a replica refuses
`WAIT`. Against Valkey, all but the snapshot's pass: Valkey's `WAIT`
returned 1 while its new replica didn't have the keys yet, which were
written before the replica connected. Valkey's `WAIT` waits for the
changes of the client that calls it, as they went to the replicas; this
one waits for every change made so far, on every shard, including those
that only a snapshot gives a new replica.

## Exercise 2

The primary's server distributes its connections by the port of the
client, with Seastar's `load_balancing_algorithm::port`: a connection
goes to the shard whose number is the remainder of the client's port,
divided by the number of shards. A replica's shard then connects from a
port whose remainder is its own number, and lands on the primary's shard
of the same number. ScyllaDB's *shard-aware* port for its CQL clients
works the same way: the client driver picks the port that lands each of
its connections on the shard that it wants. The patch is
`examples/solutions/shoal-replication-shard-port.patch`:

```{.cpp include="examples/solutions/shoal-replication-shard-port.patch" file="replication.cc" snippet="local-address"}
```

`call()` gives the client this local address; the server's options set
the algorithm. To see the difference, the primary counts the requests
that it had to pass to another shard, which `INFO` shows as
`repl_forwarded`. The first version of the test checked that the count
is 0, and it passed without the patch too: with two shards, Seastar's
default algorithm gives each new connection to the shard with the fewest,
and the replica's shard 0 happened to connect first. The test now first
opens a connection of its own to the replication port, which takes the
primary's shard 0; without the patch, the replica's connections then land
on the wrong shards, and its requests were passed on 378, 8 and 8 times,
in three runs; with it, never.

## Exercise 3

A primary listens with TLS, `tls::listen()`, and gives the server the
listening socket; a replica gives its client a TLS socket,
`tls::socket()`. The patch, `examples/solutions/shoal-replication-tls.patch`,
adds `--tls-replication`, as Redis has, and uses Shoal's certificate
options: the primary's own certificate, and the replica's, which the
primary requires, as Shoal's TLS port does:

```{.cpp include="examples/solutions/shoal-replication-tls.patch" file="replication.cc" snippet="listen"}
```

The comment in `listen()` is the trap that the exercise asked about.
`protocol::server` has two constructors that take a socket, one with the
options before the socket, and one with them after it and after the
resource limits, and the second accepts the options, and ignores them
(Seastar issue <https://github.com/scylladb/seastar/issues/3842>).
A primary that used it worked, but without compression: for 10,000 keys
with values of 100 bytes, its replica received 1,346,758 bytes, where the
patch's received 69,877, as `ss -tin` showed, which reports the bytes
that each of the replica's sockets received. The patch's test checks
that number, so it would catch the mistake, and that the primary refuses a
replica with a certificate that it doesn't trust.

The replica checks that the primary's certificate is for the name that
`--replicaof` gave, the `server_name` of its TLS options:

```{.cpp include="examples/solutions/shoal-replication-tls.patch" file="replication.cc" snippet="replica-start"}
```

## Exercise 4

The feed of the primary's shard *j* has the changes of the keys that
belong to shard *j*. On a replica with as many shards, the same keys
belong to its shard *j*, which can apply the feed by itself, in order,
without asking any other shard. With a different number of shards, the
keys of one primary shard belong to all of the replica's shards, so each
request of the feed must go to its key's shard, with `execute()` rather
than `execute_here()`, one at a time, to keep their order: a hop between
shards for most requests, which the replica's shards would also share,
since every feed reaches all of them.

The hard requests are those without a key. The primary's shard records
`FLUSHALL` when it deletes its own keys, which on such a replica are on
every shard, mixed with the keys of the primary's other shards. To
delete only them, every shard of the replica would have to find which of
its keys belong to the primary's shard *j*, by hashing each with the
primary's number of shards. The same is true when a snapshot starts:
the replica must delete the keys of the primary's shard *j* that the
primary might have deleted meanwhile, but not the others, which other
feeds are updating. Shoal 14 avoids all of this by requiring the same
number of shards, as its log already does.
