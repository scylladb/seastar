# Networking in depth {#ch:networking}

Shoal has used the network since its first version, with only a few
calls: `listen()` and `accept()` to get connections, with a few
`listen_options`; an `input_stream` to read each client's requests, with
`read()` and `read_exactly()`; an `output_stream` to write the replies,
with `write()`, `flush()` and `close()`; `set_nodelay(true)`; and, to
stop cleanly, `abort_accept()`, `shutdown_input()`, `shutdown_output()`
and `wait_input_shutdown()`. That was enough for a server that works,
and, since [chapter](#ch:designing-a-sharded-application), for one that
uses every core. This chapter looks at what those calls do, and at what
else Seastar offers:

- where connections wait before they're accepted, and the options a
  socket can be given;
- addresses, Unix-domain sockets among them, and the names that DNS
  turns into addresses;
- reading a protocol from a stream without copying it around, writing
  large replies, and clients that send faster than the server can
  answer;
- wrapping a stream in one of your own, and TLS;
- UDP;
- and, mostly for its history, Seastar's own TCP/IP stack, which
  bypasses the kernel altogether.

Shoal version 9 puts most of it to use.[^shoal-v9]

[^shoal-v9]: The source of this version is in `shoal/v9/`.

## Where connections come from {index="listen backlog"}

[Chapter](#ch:designing-a-sharded-application) gave every shard an
accept loop, with a listener of its own on the same port. With Seastar's
POSIX network stack, the one that uses the kernel's TCP/IP, there is
really one listening socket, on shard 0, which accepts every connection
and hands it to the shard chosen by the listener's load balancing
algorithm; Shoal uses the default, which picks the shard with the fewest
open connections. That's the part of the story that happens after
`accept()`. This section is about what happens before it.

When a client connects, the kernel completes TCP's handshake by itself,
without waiting for the server, and puts the new connection in a queue,
where it waits until the server's `accept()` takes it. This queue is the
listening socket's [*backlog*]{.index .main}, and the server chooses its
length when it starts listening. A Seastar server chooses it with
[`listen_options::listen_backlog`]{index="`seastar::listen_options`!`listen_backlog`" .main},
from `<seastar/net/api.hh>`, which is 100 unless the server asks for
more, and Linux silently caps it at the value of the `net.core.somaxconn`
setting, 4096 on Fedora 44. (Seastar prints a warning when it's lower
than the length asked for.)

A connection that arrives when the backlog is full doesn't fit. Linux
doesn't refuse it: it ignores the client's first packet, as if the
network had lost it, and the client's TCP sends it again when its timer
runs out, a second later. So the client connects, but after a second
instead of a fraction of a millisecond. This program shows it. A
server listens, but waits 300 milliseconds before it accepts anything,
as a server would whose shard was busy when a burst of new connections
came:

```{.cpp include="examples/networking/backlog.cc" snippet="server"}
```

Meanwhile, 300 clients connect at once, and each measures how long its
`connect()` took:

```{.cpp include="examples/networking/backlog.cc" snippet="clients"}
```

The program does this twice, first with the default backlog of 100, and
then with a backlog of 1000, set when the server starts listening:

```{.cpp include="examples/networking/backlog.cc" snippet="listen"}
```

Its output:

```
backlog 100: 101 connections in under 0.1 s, 199 took a second or more (the slowest 1.06 s)
backlog 1000: 300 connections in under 0.1 s, 0 took a second or more (the slowest 0.01 s)
```

With the default backlog, 101 clients got in at once (Linux accepts one
more than the backlog), and the other 199 waited a full second, although
the server was ready for them after 0.3 seconds
([Figure](#fig:backlog-burst)).

![How long each of 300 clients took to connect, all at once, to a server
that began to accept after 0.3 seconds, sorted from the fastest to the
slowest. With a backlog of 100, the 199 clients that didn't fit sent
their first packet again a second later; with a backlog of 1000, all of
them fit.](figures/backlog-burst.svg){#fig:backlog-burst}

With a longer backlog,
nobody waited for more than the server's delay. (The fast connections
didn't wait for the 300 milliseconds either: a client's `connect()` ends
when the handshake does, which is when the kernel puts the connection in
the backlog, not when the server accepts it.)

The backlog fills only when the server is late to accept, and a Seastar
server usually isn't: its accept loop gets to run whenever the shard's
task quota ends, every half a millisecond by default, as
[chapter](#ch:how-seastar-runs-your-code) explained. It takes a burst of
connections that come faster than that, such as a fleet of clients
reconnecting together after a network failure or a restart, or a shard
that stalls. That's what a ScyllaDB user ran into, while testing a large
cluster with many Alternator clients: one request in a thousand took
over two seconds, although ScyllaDB reported no request that took more
than half a second. The slow requests were the ones whose connection
attempts had timed out twice, after the client's timeout of a second
each, before the third attempt got in. Raising the backlog to 150 made
the failed connections disappear. (See ScyllaDB issue 18021, and the
related Seastar issue 2147. At the
time of writing, both are still open, and so are two pull requests, one
that raises Seastar's default and one that lets a server change the
backlog while it listens. In the discussion of the first, a Seastar
maintainer argued that the application should make the backlog
configurable, rather than Seastar choosing another number for
everyone.)

That's what Valkey does, with its `tcp-backlog` option, 511 by default,
and Shoal version 9 does the same, with the same option and default, in
the options it gives its listeners:

```{.cpp include="shoal/v9/main.cc" snippet="listen-options"}
```

A long backlog costs almost nothing: the kernel only allocates room for
the connections that actually wait. To see the backlog of a listening
socket, run `ss -ltn`: for a listening socket, the column `Send-Q` shows
the backlog's length, and `Recv-Q` how many connections are waiting in
it now.

## Socket options {index="socket options"}

A `connected_socket` has a few methods that set the options of the
connection. [Chapter](#ch:coroutines) explained the first one that
Shoal used, `set_nodelay(true)`: it turns off Nagle's algorithm, which
holds back a small packet while the connection's previous one hasn't
been acknowledged yet, and which, together with the client's delayed
acknowledgments, made a test program fifteen times slower. Seastar's
POSIX stack leaves Nagle's algorithm on, as the kernel does, so a server
must turn it off on every connection itself.

The second is about clients that disappear. A connection ends when one
side closes it, or when the network reports an error, but if the
client's machine crashes, or loses its network, neither happens: the
server never hears from the client again, and keeps the connection
forever, with everything that belongs to it. For Shoal, that can be a
`BLPOP` that waits forever, a subscriber whose messages pile up, and one
of the `--maxclients` connections that it allows. TCP's
[*keepalive*]{.index .main} finds such connections: when one has been
idle for a while, TCP sends the other side a probe, an empty packet that
any live TCP answers, and after enough unanswered probes it declares the
connection dead, and the server's next read or write fails. It's off
unless the program turns it on, and Linux's default timing is useless
for a server: the first probe comes after two hours of silence. So
Valkey turns it on for every client, with its option `tcp-keepalive`,
300 seconds by default: the first probe after 300 seconds of silence,
then one every 100 seconds, and the connection fails after three
unanswered probes, about ten minutes after the client went silent.
Shoal version 9 does the same, with the same option:

```{.cpp include="shoal/v9/main.cc" snippet="socket-options"}
```

[`set_keepalive(true)`]{index="`seastar::connected_socket`!`set_keepalive()`" .main}
turns keepalive on, and
[`set_keepalive_parameters()`]{index="`seastar::connected_socket`!`set_keepalive_parameters()`" .main}
sets its timing, with a
[`net::tcp_keepalive_params`]{index="`seastar::net::tcp_keepalive_params`" .main},
also from `<seastar/net/api.hh>`: how long the connection is idle before
the first probe, how long between probes, and how many probes go
unanswered before the connection fails. (Keepalive also helps with a
second problem: firewalls and NAT routers forget connections that have
been idle for a while, and break them, which the probes prevent.)

The other methods are less often needed. `shutdown_output()` and
`shutdown_input()` close one direction of the connection, as
[chapter](#ch:clean-shutdown) explained: when Shoal stops, it shuts
down each connection's input, as if the client had closed it, so that
it reads no more requests but still writes the replies, and if the
grace period ends first, it shuts down the output too. `local_address()` and
`remote_address()` return the two ends' addresses, as
`socket_address` objects. And `set_sockopt()` and `get_sockopt()` reach
any option of the kernel's socket that Seastar has no method for,
with the same arguments as the system calls `setsockopt()` and
`getsockopt()`.

## Addresses and names {index="addresses"}

Shoal has listened on `make_ipv4_address({port})` since version 0: port
6379 of every IPv4 address of the machine, written `0.0.0.0`. A
[`socket_address`]{index="`seastar::socket_address`" .main}, from
`<seastar/net/socket_defs.hh>`, can hold any of the three kinds of
address this chapter uses: an IPv4 address and port, an IPv6 address
and port, or a Unix-domain socket's file name. An IP address, without a
port, is a
[`net::inet_address`]{index="`seastar::net::inet_address`" .main}, from
`<seastar/net/inet_address.hh>`, which holds either version.

Valkey chooses its addresses with its `bind` option, and Shoal version 9
does too, with `--bind`, which takes one or more addresses, `0.0.0.0` by
default. Listening on `::`, the IPv6 address that means "all of them",
accepts IPv6 clients, and, as Linux does by default, IPv4 clients too.
And like Valkey's, Shoal's `--bind` also takes names, such as
`localhost`, which must be turned into addresses: by the file
`/etc/hosts`, or by asking a DNS server.

That's a problem for a Seastar program. The usual way to resolve a name,
the C library's `getaddrinfo()`, blocks until the answer comes, which
from a DNS server across the network may take milliseconds, or seconds
if the server doesn't answer and the query must be retried; and a shard
that blocks does nothing else meanwhile. So Seastar has a resolver of its
own,
[`net::dns_resolver`]{index="`seastar::net::dns_resolver`" .main}, from
`<seastar/net/dns.hh>`, built on the c-ares library, which sends its
queries over Seastar's own network stack, and returns futures. The
functions in the namespace `net::dns` use a resolver of the shard's own,
with the system's settings from `/etc/resolv.conf`. Version 9 resolves
`--bind`'s names with one of them:

```{.cpp include="shoal/v9/main.cc" snippet="resolve"}
```

[`get_host_by_name()`]{index="`seastar::net::dns::get_host_by_name()`" .main}
returns all of a name's addresses, each with how long DNS allows it to be
remembered, its *time to live*; `resolve_name()` returns only the first.
There are also `get_host_by_addr()` and `resolve_addr()`, which find the
name of an address, and `get_srv_records()`, which finds the servers of
a service, as DNS's SRV records list them. `/etc/hosts` is read first,
and with ordinary reads, which block; Seastar assumes, as a comment in
its source says, that reading a small local file won't block for long.

Shoal resolves the names once, on shard 0, before it starts its
services, which copy the options, addresses included, to every shard, so
that all the shards listen on the same addresses even if DNS gave each a
different answer. With `--bind localhost`, Shoal listens on both
`127.0.0.1` and `::1`, the two addresses `/etc/hosts` gives
`localhost`; Valkey listens on only the first address of a name. And a
name that doesn't exist stops Shoal as it starts:

```
ERROR [shard 0:main] seastar - Exiting on unhandled exception: std::system_error (error C-Ares:4, no.such.name.invalid: Not found)
```

ScyllaDB resolves the addresses in its configuration, such as its
`listen_address` and its seeds, with the same functions, and uses them to
find its LDAP and KMIP servers too.

A client on the same machine as the server can skip TCP and IP
altogether, with a third kind of address: a
[*Unix-domain socket*]{.index .main}, a socket that
has a file name instead of an address and a port, through which the
kernel passes the data directly from one process to the other. Seastar
supports them with the same `listen()`, given a `socket_address` made
from a [`unix_domain_addr`]{index="`seastar::unix_domain_addr`" .main},
from `<seastar/net/unix_address.hh>`, and everything else is the same as
with TCP. (`set_nodelay()` and the keepalive methods do nothing on such
a socket, since there is no TCP to configure.) Shoal version 9 listens
on one if given Valkey's option, `--unixsocket`, as the TLS section's accept loop
shows; like Valkey, it first removes the socket's file, which an
earlier run may have left behind, since an existing file can't be
listened on.

Two more options of `listen_options` concern addresses. A server behind
a load balancer, such as HAProxy or the load balancers of the clouds,
sees every connection come from the load balancer's address, unless the
load balancer passes on the client's, in a short header at the start of
the connection: the *PROXY protocol*. With
[`listen_options::proxy_protocol`]{index="`seastar::listen_options`!`proxy_protocol`" .main},
Seastar reads that header itself, and `remote_address()` returns the
client's address. ScyllaDB has a port of this kind for each of its CQL
ports, such as `native_transport_port_proxy_protocol`. Among them is a
*shard-aware* one, whose listener uses
[chapter](#ch:designing-a-sharded-application)'s `port` algorithm, which
gives each connection to the shard that the client's port chooses, so
that a client can open a connection to the very shard that owns the data
it wants. And `listen_options::proto` chooses the transport protocol:
TCP, by default, or SCTP, a protocol of connections that carry separate
messages, used mostly in telephone networks, which Seastar supports in
its POSIX stack, and which `connect()` takes too. Seastar's API has no
raw sockets, which send and receive whole IP packets, and the class that
its POSIX stack uses to wait for its sockets, `pollable_fd`, is in
`<seastar/core/internal/pollable_fd.hh>`, one of its internal headers,
not its public API.

## Reading a protocol from a stream {index="parsing"}

Shoal's request reader has hardly changed since version 1, in
[chapter](#ch:coroutines). It keeps the data that it has read but not
used yet in a `temporary_buffer<char>`, and reads a request in pieces:
`read_line()` finds the end of a line such as `*3` or `$5` in the
buffer, copies the line into an `sstring`, and parses the number in it,
and `read_exactly()` copies an argument out of the buffer. When the
buffer runs out in the middle of a line, `read_more()` reads more from
the stream, and joins it to what was left of the line, copying both.
(An argument that the buffer cuts in two needs no join:
`read_exactly()` copies what the buffer has of it into its `sstring`,
and the rest straight from the stream.) [Chapter](#ch:coroutines) noted that this joining isn't free,
since with pipelining most reads end in the middle of a request, and
promised a reader that parses the stream's buffers as they are.

Before writing one, we measured what the joining costs. With
`valkey-benchmark` pipelining small `SET`s to version 8 on one shard,
`perf record` found the reader taking about 35% of the shard's time:
16% in `read()` itself, 8% in `read_line()`, 6% in parsing the numbers,
and 5% in `read_exactly()`. Copying memory, the joins included, took
only 2%, and even with values of 1,000 bytes, only 5%. So the copying
that [chapter](#ch:coroutines) worried about was a small part of the
cost. The large part was the machinery around it: a coroutine for every
request, an `sstring` for every line, and a search for every line's end
before its number is parsed.

### The input stream's ways to read

An `input_stream` offers four ways to read, which differ in who decides
how much is read. [Chapter](#ch:coroutines) showed two of them:
`read()` returns whatever the stream has, its buffer or the next piece
that the connection delivers, and `read_exactly(n)` returns `n` bytes,
joining pieces if it must. The third,
[`read_up_to(n)`]{index="`seastar::input_stream`!`read_up_to()`" .main},
returns at most `n` bytes, and never joins: it returns part of the
stream's buffer, with `share()`, if that buffer is longer. The fourth,
[`consume()`]{index="`seastar::input_stream`!`consume()`" .main}, hands
the stream's buffers, one at a time, to a *consumer* of our own: a
function that takes a `temporary_buffer<char>`, and returns a future of a
[`consumption_result<char>`]{index="`seastar::consumption_result`" .main},
also from `<seastar/core/iostream.hh>`, which says what to do next:

- [`continue_consuming`]{index="`seastar::continue_consuming`" .main}:
  the consumer used the whole buffer, and wants the next one.
- [`stop_consuming(rest)`]{index="`seastar::stop_consuming`" .main}: the
  consumer is done, and `rest`, the part of the buffer it didn't use,
  goes back to the stream, to be read first the next time.
- [`skip_bytes(n)`]{index="`seastar::skip_bytes`" .main}: the consumer
  used the whole buffer, and the stream should skip the next `n` bytes
  without reading them into memory, for a protocol that has data to
  ignore.

`consume()` ends when the consumer says to stop, or at the end of the
stream, after the consumer has received an empty buffer. This is the
interface for a parser that keeps its own state, and can stop at any
byte, in the middle of anything, and resume when the next buffer comes.
That's how Seastar's memcached server, in its `apps/memcached/`, reads
its requests, with a parser generated by the Ragel parser generator.
ScyllaDB's CQL server, whose protocol begins every request with its
length, does something simpler: it reads a request's header with
`read_exactly()`, and then the whole request, into a buffer made of
pieces, so that it isn't copied into one.

### A parser that keeps its place

Shoal version 9's reader parses each buffer that `consume()` hands it
into all the requests the buffer holds. A request that the buffer cuts
in two stays half-parsed in the reader, which knows where it was, and
continues from there when the next buffer comes
([Figure](#fig:parser-buffers)):

![Two pipelined requests, which the stream delivers in three buffers,
cut anywhere: here in the middle of the line `$3`, and in the middle of
the argument `hello` (each $\hookleftarrow$ is a `\r\n`). Version 8
joins the line's beginning to the next buffer, copying both; version 9
never joins, but keeps its place at each cut.](figures/parser-buffers.svg){#fig:parser-buffers}

```{.cpp include="shoal/v9/resp.hh" snippet="reader"}
```

`read()` returns the next request. Usually the last buffer held more
requests than one, and it returns the next of them at once, without a
coroutine; only when there are none does it call `read_slow()`, which
parses what the client sends next:

```{.cpp include="shoal/v9/resp.cc" snippet="read"}
```

The consumer is a lambda that calls `parse_buffer()`. It throws a
`protocol_error` when the client breaks the protocol, and
[`futurize_invoke()`]{index="`seastar::futurize_invoke()`" .main}, from
`<seastar/core/future.hh>`, turns that into a failed future, which is
what `consume()` expects of a consumer that fails. `parse_buffer()`
tells the stream what to do next:

```{.cpp include="shoal/v9/resp.cc" snippet="parse-buffer"}
```

When it has whole requests, it stops, and gives the rest of the buffer
back to the stream, with `trim_front()`, which copies nothing. The next
`read_slow()` calls `consume()` again, and the stream hands that rest to
the parser first. The parser itself walks through the buffer one
character at a time, except in an argument, which it copies as a whole,
straight from the buffer into the argument's `sstring`, the only copy
the argument needs:

```{.cpp include="shoal/v9/resp.cc" snippet="parse"}
```

A number is parsed as its digits arrive, so it can be split between
buffers as well:

```{.cpp include="shoal/v9/resp.cc" snippet="parse-number"}
```

And the request grows one argument at a time:

```{.cpp include="shoal/v9/resp.cc" snippet="arguments"}
```

One thing doesn't change. Version 6, in [chapter](#ch:coordination),
limited the memory of large requests, those whose arguments add up to
more than 64 KB, with a semaphore: a request must get units from it
before the argument that brings it over is allocated, and later
arguments take more units without waiting. The parser can't wait for a
semaphore in the middle of a buffer, so it stops instead: in the `admit`
state, `parse_buffer()` returns the rest of the buffer to the stream, and
`read_slow()` waits for the units, allocates the argument, and only then
lets the parser continue. Taking more units, without waiting, it can do
in the middle of a buffer, and does, in `admit_more()`.

How much did all this gain? On one shard, with the benchmark of
[chapter](#ch:designing-a-sharded-application), the best of three
runs:

| Millions of requests a second | Version 8 | Version 9 |
|---|---|---|
| `SET`, random keys | 1.26 | 1.45 |
| `GET`, random keys | 1.28 | 1.40 |
| `SET`, one key | 3.35 | 5.03 |
| `GET`, one key | 3.75 | 5.31 |
| `SET`, one key, values of 1,000 bytes | 1.82 | 2.01 |
| `SET`, one key, values of 100,000 bytes | 0.050 | 0.050 |

Without the coroutine and the strings, a request costs much less to
read. With one key, where the hash table costs least, Shoal on one shard
became about half again as fast; with random keys, where the hash
table's cache misses cost most, as
[chapter](#ch:designing-a-sharded-application) measured, 9% to 15%.
With values of 100,000 bytes, there is no difference, although version 9
copies each value once, where version 8 copied it twice: at five
gigabytes a second, something else sets the pace, the kernel's copying
or the client. (We didn't look into which.)

## Writing large replies {index="zero-copy writes"}

An output stream, as [chapter](#ch:coroutines) showed, collects what the
program writes in a buffer, and sends the buffer when it's full or
flushed. That's two kinds of `write()`. The *copying* ones take a string,
an `sstring` or a `std::string`, or a pointer and a length, and copy the
bytes into the stream's buffer, so the program may reuse or free its
string as soon as `write()` returns; they suit replies made of many small
pieces. The *zero-copy* ones take a `temporary_buffer<char>`, or a
`std::span` of them, and keep the buffer itself, sending it as it is,
without copying a byte; they suit large data that is already in buffers.
The stream sends its own buffer, and the program's, in the order they
were written. One rule comes with mixing them: after a zero-copy write,
the stream must be flushed before the next copying write.

Two more facts about the stream matter here. First, `flush()` doesn't
wait for anything. A socket's output stream is created with
`output_stream_options::batch_flushes`, which makes `flush()` only mark
the stream as needing a flush, and return at once; the reactor sends
the data later in the same turn of its loop, together with the other
connections' data. So the future of `flush()` is ready, and only a
`write()` waits, when the stream has a buffer to send, and the previous
one is still being sent, because the kernel's socket buffer is full,
because the client isn't reading. Second, a `write()` that waits has
already copied its data: the future says only when the stream is ready
for the next call. All of a stream's methods must be called one after
another, never before the previous one's future is resolved.

Shoal's replies are `sstring`s, and almost all of them are small: `+OK`,
an integer, a value. Two kinds can be large. A value, which can be up to
512 MB, is returned by `GET` from the `sstring` that holds it, and
written by a copying `write()`. And a reply made of many parts, such as
`KEYS`'s, which version 8 built by appending every key to a
`std::string`, and then copied into an `sstring`: a reply that grows by
doubling its memory, copying everything each time, then copied once more
as a whole, needing room for two copies, in two large pieces of
contiguous memory, built without yielding. With a million keys, the
reply is about 17 MB, and while version 8 built it, the shard did
nothing else: a `PING` from another client, sent during the `KEYS`,
waited up to 50 ms for its reply.

Version 9 builds `KEYS`'s reply as Valkey builds all its replies: in
pieces of 16 KB, each a `temporary_buffer<char>`:

```{.cpp include="shoal/v9/resp.hh" snippet="reply-builder"}
```

```{.cpp include="shoal/v9/resp.cc" snippet="reply-builder-def"}
```

A piece, once full, is never copied again: not when the reply grows, and
not when it's written, since `write_reply()` hands each piece to the
stream's zero-copy `write()`. The commands that are executed alone, such
as `KEYS`, return a `reply`, which is either kind, and `KEYS` yields
while it builds its reply, so that the shard's other connections aren't
stalled while it formats a million keys:

```{.cpp include="shoal/v9/commands.cc" snippet="keys-reply"}
```

With version 9, the `PING` during the `KEYS` waited at most 5 ms.

## Clients that send faster than we answer {index="backpressure"}

[Chapter](#ch:coroutines)'s exercise 6 sent a million `APPEND`s to Shoal
in one pipeline, with redis-py, which sends a whole pipeline before it
reads any reply. Valkey finished in a few seconds; Shoal got stuck. Its
solution looked at the stuck connection with `ss -tn`, and found both
directions full: megabytes of requests that Shoal hadn't read, and
megabytes of replies that the client hadn't read. Shoal wrote a reply
after every request, and once the socket's buffers were full,
`out.write()` waited for the client to read, and Shoal read nothing
until it did. The client read nothing until it had sent everything, and
couldn't send more until Shoal read. Each waited for the other
([Figure](#fig:pipeline-deadlock), top).

![A client that sends a huge pipeline, and reads its replies only after
sending all of it. In version 8 (top), the sockets' buffers fill up in
both directions: the client waits for Shoal's reader, which waits for
`write()`, which waits for the client. In version 9 (bottom), the reader
goes on, and the replies wait in Shoal's memory, up to a limit, until
the client has sent everything, and reads.](figures/pipeline-deadlock.svg){#fig:pipeline-deadlock}

That was a deliberate choice, with a cost on each side. A server that
stops reading a client whose replies it can't send applies
[*backpressure*]{.index .main}: a client that doesn't read its replies
slows only itself, and the server never holds more than a socket buffer
of replies for it. A server that goes on reading, as Valkey does, must
keep the replies it can't send yet somewhere, in its memory, and for an
ordinary client, Valkey sets no limit on how much: a client that sends
requests and never reads the replies can make it use all its memory.
Real clients avoid the problem either way, by sending their pipelines in
batches of moderate size, reading each batch's replies before sending
the next, as the documentation of Redis recommends, or by reading while
they write, as `valkey-benchmark` does. But a client library that sends
a whole pipeline before reading, as redis-py does, is common enough that
a server should serve it.

Shoal version 9 takes the middle road. It keeps reading while replies
wait, and it counts the waiting replies' memory, with a semaphore for
each shard, set by a new option, `--max-reply-memory`, by default an
eighth of the shard's memory. Only when the shard's waiting replies use
all of it does Shoal stop reading, from every client of the shard, until
the clients whose replies wait read some of them. Stopping everyone is
blunt, but it's simple, and it only happens when the shard is about to
run out of memory. That's the pattern of
[chapter](#ch:concurrency-within-a-shard)'s Pub/Sub, which bounds the
memory of unsent messages the same way.

The replies already had a queue: [chapter](#ch:designing-a-sharded-application)'s
`ordered_replies`, whose writer writes them in order. What needs to
change is who waits for the client. In version 8, `serve()` itself
waited, in its fast path, which wrote a local request's reply with
`co_await out.write()`, and in `reserve()`, whose units each reply held
until it was written, so that with 128 replies waiting, `serve()` waited
too. In version 9, only the writer waits ([Figure](#fig:pipeline-deadlock),
bottom). The queue's entries change: a ready reply carries the units of
its bytes in the shard's reply memory, which it holds until it's
written, and the request's memory, as before:

```{.cpp include="shoal/v9/main.cc" snippet="reply-entries"}
```

The rest of the class is as it was, except for three new methods, and
`add()`, which now gives back the unit of `reserve()` as soon as the
reply is ready, rather than when it's written:

```{.cpp include="shoal/v9/main.cc" snippet="add-write-now"}
```

`write_now()` is the fast path. It writes a ready reply without a
coroutine, and almost always that's the end of it: the copying
`write()` copies the reply into the stream's buffer, and `flush()` only
asks the reactor to flush. When the stream can't take more, because the
client isn't reading, `write()` returns a future that isn't ready, and
`write_now()` hands it to a writer, which waits for it, and then writes
the replies that came meanwhile, from the queue. A reply in the queue
gives back its unit of `reserve()` as soon as it's ready, rather than
when it's written, so the limit of 128 now counts only the requests
being executed, and a ready reply takes its bytes from the shard's reply
memory, without waiting, until it's written. The writer itself
changes only at its start: it waits first for the write before it, the
one `write_now()` handed it, if any:

```{.cpp include="shoal/v9/main.cc" snippet="write-before"}
```

In `serve()`, the fast path tries `write_now()`, and puts the reply in
the queue only if it must:

```{.cpp include="shoal/v9/main.cc" snippet="fast-path"}
```

And `serve()` waits for the shard's reply memory before it reads each
request:

```{.cpp include="shoal/v9/main.cc" snippet="serve-read"}
```

With this, a million `RPUSH`es in one pipeline, sent before any reply is
read, get their replies, as one of version 9's tests checks. (Version 9
has no `APPEND`; the exercise had readers add it.) The commands that
are executed alone, such as `KEYS`, and Pub/Sub's, still wait for the
earlier replies to be written, as exercise 2 points out.

## Your own data source and sink {index="`seastar::data_source`"}

An `input_stream` doesn't read from the network itself. It reads from a
[`data_source`]{index="`seastar::data_source`" .main}, and an
`output_stream` writes to a
[`data_sink`]{index="`seastar::data_sink`" .main}, both from
`<seastar/core/iostream.hh>`. The streams do the buffering, and the
source and sink move the data: a socket's source returns the buffers
that the kernel delivers, and its sink hands buffers to the kernel. A
source or sink is a small object that holds a pointer to an
implementation, a class that derives from
[`data_source_impl`]{index="`seastar::data_source_impl`" .main} or
[`data_sink_impl`]{index="`seastar::data_sink_impl`" .main}, and that's
where a program can put its own: a source that decompresses another
source, a sink that encrypts, or one that counts. The streams work the
same over any of them, so whatever reads or writes the stream, such as
Shoal's parser and its replies, doesn't need to know.

Shoal version 9 uses this to count the bytes its clients send and
receive, which Valkey's `INFO` command reports as
`total_net_input_bytes` and `total_net_output_bytes`, and which version
9's new `INFO` reports too. The source and the sink each wrap another,
and count what passes through:

```{.cpp include="shoal/v9/main.cc" snippet="counting-streams"}
```

A source must implement `get()`, which returns the next buffer, or an
empty one at the end, and may implement `skip(n)` and `close()`. A sink
must implement `put()`, which takes a span of buffers, and `close()`,
and since an output stream asks its sink how large its buffers should be
and whether its flushes may be batched, the counting sink asks the sink
it wraps.

To use them, `handle_connection()` takes the socket's streams apart,
with [`detach()`]{index="`seastar::input_stream`!`detach()`" .main},
which returns a stream's source or sink, and builds new streams around
the wrapped ones:

```{.cpp include="shoal/v9/main.cc" snippet="wrap-streams"}
```

Everything else in Shoal uses `in` and `out` as before. ScyllaDB does
exactly this in its CQL server, whose connections wrap the socket's
source and sink in a `counted_data_source_impl` and a
`counted_data_sink_impl`, for a different count: each connection holds
units of a semaphore that limits how many requests use the CPU at once,
and its wrappers give the units back while the connection waits for the
network, and take them again when it's done.

## TLS {index="TLS"}

Clients that talk to a server over a network they don't trust need
[*TLS*]{.index .main}, Transport Layer Security, which encrypts the
connection, and proves to the client that it reached the real server,
with the server's certificate; and with a certificate of its own, the
client can prove who it is too. Redis has supported TLS since version 6,
and Valkey's options for it are `tls-port`, a port of its own for TLS
clients, `tls-cert-file` and `tls-key-file`, the server's certificate and
private key, `tls-ca-cert-file`, the certificates that client
certificates must be signed by, and `tls-auth-clients`, whether clients
must have one. Shoal version 9 has the same options.

Seastar's TLS is in `<seastar/net/tls.hh>`, and comes in two
implementations, one using the GnuTLS library and one using OpenSSL. A
Seastar build includes one of them or both, and the program doesn't need
to know which: with both, the option
[`--crypto-provider`]{index="command-line options!`--crypto-provider`"}
chooses when the program starts, GnuTLS by default. A server describes its certificate to a
[`tls::credentials_builder`]{index="`seastar::tls::credentials_builder`" .main},
builds its *credentials* from it, and listens with
[`tls::listen()`]{index="`seastar::tls::listen()`" .main}, which takes
the credentials, and returns a `server_socket` like `listen()`'s. The
connections it accepts are `connected_socket`s like any others, whose
streams encrypt and decrypt: the TLS handshake happens on the first read
or write, so the rest of Shoal doesn't change at all. Version 9's accept
loop now listens on a TCP socket and a TLS one for each address of
`--bind`, and on a Unix-domain socket, and accepts from all of them:

```{.cpp include="shoal/v9/main.cc" snippet="accept"}
```

Every shard has a listener, and needs credentials of its own, since
Seastar's objects belong to one shard. A shard builds its credentials
before it starts accepting:

```{.cpp include="shoal/v9/main.cc" snippet="tls-credentials"}
```

Shard 0 builds them differently, with
[`build_reloadable_server_credentials()`]{index="`seastar::tls::credentials_builder`!`build_reloadable_server_credentials()`" .main},
which also watches the certificate's files, and when they change,
reloads them, and calls the function it was given, with a builder that
holds the new files. Certificates expire, often after a few months, and
are renewed by a program that writes the new certificate over the old
one. A server that reads it only when it starts keeps presenting the old
one, until it's restarted, or until it expires and clients refuse it.
With reloadable credentials, Shoal's new connections get the new
certificate as soon as it's written, and the old connections, which
already did their handshake, go on undisturbed. The function rebuilds
the other shards' credentials from shard 0's builder, so that only one
shard watches the files. Every shard could watch them itself, with less
code, but each watcher costs one of Linux's *inotify* instances, of which
a user may have only 128, by default. ScyllaDB learned this the hard
way: with TLS on two of its ports, it couldn't start on more than 64
shards (ScyllaDB issue 7700, in 2020), and after raising the limit in the
system settings it installs, it ran out again on busy test machines,
and moved, in 2025, to the design that Shoal uses here. (Valkey 9 reloads its certificate when the
administrator asks, with `CONFIG SET`. Version 9's test of reloading does
that too, and Shoal ignores it.)

What does TLS cost? On one shard, with the benchmark of the parser
section, and the same for Valkey, the best of three runs, all four
columns measured together on one night (the plain Shoal 9 numbers
differ from the table above by up to 2%, as runs on different nights
do):

| Millions of requests a second | Shoal 9 | Over TLS | Valkey | Over TLS |
|---|---|---|---|---|
| `SET`, random keys | 1.45 | 1.38 | 1.96 | 1.86 |
| `GET`, random keys | 1.39 | 1.34 | 2.96 | 2.72 |
| `SET`, one key | 4.96 | 4.33 | 2.59 | 2.44 |
| `GET`, one key | 5.21 | 4.67 | 3.30 | 3.09 |

Much less than one might fear. Encrypting a byte takes the processor a
fraction of a nanosecond, with the instructions that modern processors
have for it, and with 64 requests in a pipeline, each piece that TLS
encrypts holds many requests. So with random keys, where most of the
time goes to the hash table, TLS costs Shoal 4% to 5%, and with one
key, 10% to 13%. What TLS does cost is the handshake that begins
every connection, which a benchmark of long connections doesn't see: a
client that opens a connection for each request pays it every time.

## UDP {index="UDP"}

Besides TCP's connections, a Seastar program can use UDP's
*datagrams*: messages sent one by one, without a connection, which arrive whole, or not at all, and not
necessarily in the order they were sent. DNS is the best-known protocol
that uses them. Seastar's are
[`datagram_channel`]{index="`seastar::net::datagram_channel`" .main}s,
made by
[`make_bound_datagram_channel()`]{index="`seastar::make_bound_datagram_channel()`" .main},
for a channel with an address of its own, which a server needs, or by
[`make_unbound_datagram_channel()`]{index="`seastar::make_unbound_datagram_channel()`" .main},
for one whose address the kernel chooses, which a client may use; both
come from `<seastar/core/seastar.hh>`. A channel's `receive()` returns
the next datagram, with its sender's address, and its `send()` sends one
to an address. This program's server answers each datagram it receives
with the same text in capital letters:

```{.cpp include="examples/networking/udp.cc" snippet="server"}
```

And its client sends a few words:

```{.cpp include="examples/networking/udp.cc" snippet="client"}
```

Its output:

```
sent hello, received HELLO from 127.0.0.1:42236
sent datagram, received DATAGRAM from 127.0.0.1:42236
```

The program is so short because nothing goes wrong on one machine. Over
a real network, a datagram can be lost, so a real client must give up
waiting for an answer after a while, and send its request again, which
the tools of [chapter](#ch:time) do.

## Seastar's own TCP/IP stack {index="native network stack"}

Everything so far used Seastar's [*POSIX network stack*]{index="POSIX network stack" .main},
the default, which works through the kernel's sockets: the kernel's
TCP/IP receives the packets, and Seastar reads and writes the data with
system calls, as any program does. Seastar also has a second stack, the
[*native stack*]{.index .main}, chosen with
[`--network-stack native`]{index="command-line options!`--network-stack`"}:
its own implementation, in user space, of Ethernet, ARP, IPv4, ICMP, UDP,
TCP and DHCP, which sends and receives raw packets, and bypasses the
kernel's network code altogether. It's as old as Seastar itself: its TCP
was written in 2014, in Seastar's first months, when its authors set out
to build servers that handle millions of requests a second, and saw the
kernel, with its system calls, its interrupts, and its data shared by
all the cores, as the bottleneck. For years it was Seastar's best-known
feature. Today it's mostly of historical interest, and this section
explains both: what made it attractive, and why the kernel won anyway.
We don't recommend using it, and we haven't run it ourselves.

### How it works

The native stack gets its packets from a network card driven directly
from user space by [DPDK]{.index .main}, the Data Plane Development Kit,
a library of drivers that take over a network card from the kernel.
Every shard has its own queue on the card, for the packets it sends and
for those it receives. A modern card has many such queues, and decides
which queue receives each packet by a hash of its addresses and ports,
the card's
[*receive-side scaling*]{index="receive-side scaling (RSS)" .main},
or RSS. All the packets of a TCP connection hash alike, so they all
reach the same shard, which runs TCP for that connection, from the
handshake to the last packet, alone. Nothing is shared between the
cores: no locks, no data moving between caches, and no system calls,
since the shard polls its queues as part of its reactor loop, as
[chapter](#ch:how-seastar-runs-your-code) showed it polls everything
else, so no interrupts either. Received data can even reach the
program's `input_stream` without being copied: when Seastar's memory
comes from huge pages (the option `--hugepages`), the network card
writes each packet straight into Seastar's memory, and the
`temporary_buffer` that `read()` returns points into it.

This is shared-nothing taken all the way down to the hardware, and it
changes where connections live. A connection belongs to the shard whose
queue the card chose for it, by the hash, and not to the shard that the
listener's load balancing algorithm would choose. In the other
direction, when a shard connects to a server, Seastar picks a source
port whose hash brings the replies back to that shard's queue. The
program doesn't change: `listen()`, `connect()` and the streams work the
same with both stacks.

### Why the kernel won

The native stack's promise was speed, and it can deliver. The most
careful public comparison we know of was written by Marc Richards in
2022, in work sponsored by ScyllaDB, as his article says. He measured a
minimal Seastar server that answers every request with the same HTTP
reply, on AWS machines with 4 cores, with 256 client connections and no
pipelining, and published every step:

| Seastar's stack | Requests a second |
|---|---|
| POSIX, Linux as it came | 358,000 |
| POSIX, Linux and Seastar tuned | 1,008,000 |
| Native, DPDK | 1,191,000 |
| Native, DPDK, with "write combining" for the card | 1,515,000 |

DPDK was four times faster than Linux as it came, but tuning Linux, and
Seastar's own options, closed most of the gap: in the end, DPDK was 1.5
times faster. That's a real gain, for a server whose work is little more
than moving packets. But it took a second network card for DPDK, a DPDK
driver patched by hand, and an unsafe mode of the kernel's driver for
user-space devices, since AWS's virtual machines lack the hardware that
makes it safe. And the more work a server does per request, the less its
network stack matters: a database like ScyllaDB spends most of its time
on its own work, and Shoal, as
[chapter](#ch:designing-a-sharded-application) measured, spends a large
part of its time on its hash table.

Against that gain stand the native stack's costs. The program owns the
card, which must be dedicated to it, so the machine needs another card
for everything else, such as `ssh`; and the kernel's tools don't see its
traffic: no `tcpdump`, no `ss`, no firewall rules. It supports only one
card, and only IPv4. Every shard polls its queue all the time, so the
program uses all its cores fully even when idle. And Seastar's TCP is a
much smaller implementation than the kernel's, used by far fewer
programs, so it has had far less testing against the world's variety of
networks and clients: in the four years before this book was written,
its main files changed in 28 commits, most of them cleanups made all
over Seastar at once.

ScyllaDB's own history shows where things ended up. Its packages still
include the scripts that bind a card to DPDK, and its documentation
still describes DPDK as "an enormous performance boost". But its
packages run it with the POSIX stack, and in March 2025 its developers
stopped building it with DPDK at all, because, as the commit says,
"DPDK is not used in production and increases build time". For a new
Seastar server, the POSIX stack is the right choice, and it's the one
this book uses.

For readers who want to try the native stack anyway, the parts are: a
Seastar built with DPDK, as [appendix](#app:building-seastar) showed; a
network card that DPDK supports, dedicated to the program, and bound to
the kernel's `vfio-pci` driver with DPDK's `dpdk-devbind.py`; huge pages,
reserved with `dpdk-hugepages.py` (both scripts are in Seastar's
`dpdk/usertools/`); and the options `--network-stack native --dpdk-pmd
--hugepages /dev/hugepages`, with the program's address from DHCP, or
given with `--host-ipv4-addr`, `--netmask-ipv4-addr` and
`--gw-ipv4-addr`. A machine with several cards describes each in YAML,
with `--net-config`, or `--net-config-file` for a file: its PCI address
(`pci-address`) or DPDK port (`port-index`), its address or `dhcp`, and
the offloads it should use. (Seastar's `doc/network-configuration.md`
describes this, but writes the first two keys with underscores, which
the parser rejects.) Seastar's `doc/native-stack.md` also shows how to run
the native stack, without DPDK, over a Linux *tap* device, a virtual
network card, which is enough to experiment with its TCP. All of it
needs root. Seastar's DPDK code was tuned for Intel's server cards (its
source has workarounds for the `ixgbe` and `i40e` families, and for
VMware's virtual `vmxnet3`); our laptop's only wired card is a desktop
chip with a single queue, which is one reason we didn't try.

## Exercises

1. Make `backlog.cc`'s server wait five seconds before it accepts,
   instead of 300 milliseconds, and while it waits, look at its
   listening socket with `ss -ltn`, and at the clients' sockets with
   `ss -tn state syn-sent`. What do the numbers mean? Then make the
   clients connect one at a time, each after the previous one
   connected: what changes, and why?

2. Send version 9 one pipeline, before reading any reply, of a thousand
   `GET`s of a value of 10 KB, then a `DBSIZE`, then a thousand `SET`s
   of values of 100 KB. What happens, and why? Why doesn't it happen
   without the `SET`s? Change `serve()` so that a command that is
   executed alone waits only for the earlier requests to be executed,
   not for their replies to be written.

3. Seastar has two implementations of TLS, which `--crypto-provider`
   chooses between. Measure Shoal's speed over TLS with each, with the
   benchmark of the TLS section. Which is faster, and by how much? With
   `perf record`, find where each spends its time.

4. With `--tls-auth-clients yes`, every TLS client has a certificate.
   Log the subject of each client's certificate when it connects, with
   `tls::get_dn_information()`, and check it with a client certificate
   of your own, made with `openssl` as `test_v9.py` makes them.

5. Make `udp.cc`'s server ignore every other datagram, as a network that
   loses some would. What does the client do? Make it send its datagram
   again every 100 milliseconds, until an answer comes. Can an answer
   now come twice, and what should the client do about it?
