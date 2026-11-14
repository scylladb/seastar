# Networking in depth {#sol:networking}

## Exercise 1

The program is a copy of `backlog.cc` whose server waits five seconds,
and which, with `--one-at-a-time`, connects its clients one after
another, with `max_concurrent_for_each()` and a limit of one:

```{.cpp include="examples/solutions/networking-ex1.cc"}
```

Two seconds into its burst, `ss -ltn` showed its listening socket (with
the other sockets left out):

```
State  Recv-Q Send-Q Local Address:Port  Peer Address:Port
LISTEN 101    100        127.0.0.1:46949      0.0.0.0:*
```

For a listening socket, `Send-Q` is the backlog's length, 100, and
`Recv-Q` is how many connections wait in it: 101, one more than the
backlog, as the chapter found. And `ss -tn state syn-sent` listed 199
sockets, the clients whose connection Linux ignored: they sent their
first packet, the SYN, got no answer, and wait to send it again. (There
were also 202 established sockets: both ends of the 101 connections in
the backlog. A connection in the backlog is complete; only the server
doesn't know about it yet.) The output, at the end:

```
backlog 100: 101 connections in under 0.1 s, 199 took a second or more (the slowest 5.17 s)
```

With the clients one at a time, the output was:

```
backlog 100: 299 connections in under 0.1 s, 1 took a second or more (the slowest 5.12 s)
```

The first 101 clients connected at once, and filled the backlog. The
102nd found it full, and waited; and since the clients connect one at a
time, everyone else waited behind it, without trying. When the server
started accepting, after five seconds, the 102nd client got in at its
next try, and the rest followed at once. Only one connection was slow,
but all of them were late.

Why 5.12 seconds, and not a whole number of seconds after a timeout that
doubles each time? Linux retries a SYN after a second, and then, for the
first four retries, a second apart again, before the waits start
doubling (the setting `net.ipv4.tcp_syn_linear_timeouts`, 4 here). So the
102nd client tried at 1, 2, 3, 4 and 5 seconds, and its fifth try came
just after the server began to accept.

## Exercise 2

It gets stuck. The `GET`s' replies, 10 MB in all, are more than the
sockets can hold, so the writer waits for the client to read them. Then
`serve()` reads the `DBSIZE`, which is executed alone, after
`replies.drain()`, which waits for the writer, so `serve()` stops reading.
But the client is still sending its `SET`s, 100 MB of them, and won't
read a reply until it has sent them all: a deadlock again, as in
[chapter](#ch:coroutines)'s exercise 6. Without the `SET`s, the client
would have finished sending before `serve()` waited, and gone on to read
the replies, which would have let the writer, and then `DBSIZE`, finish.

A command that is executed alone must come after the earlier requests,
but it doesn't have to wait for their replies to be written. The
solution counts the requests whose replies aren't ready yet, and waits
until there are none; then it executes the command, and puts its reply
in the queue, after the others, which now holds a `reply`, a string or
pieces, rather than only an `sstring`. The solution is a patch to
version 9, `examples/solutions/shoal-networking.patch`:

```{.cpp include="examples/solutions/shoal-networking.patch" file="main.cc" snippet="wait-executed"}
```

```{.cpp include="examples/solutions/shoal-networking.patch" file="main.cc" snippet="alone"}
```

The writer writes the pieces of a reply such as `KEYS`'s without copying
them, so it now flushes after such a reply, as the output stream
requires before the next copying write. A new test, with its `DBSIZE` between the `GET`s and the `SET`s, passes
against the solution and against Valkey, and times out against version
9:

```{.python include="examples/solutions/shoal-networking.patch" file="test_solutions.py"}
```

A first version of the test had a million `RPUSH`es, a `DBSIZE`, and a
million more, and it showed two other things. Version 9 passed it: the
second million `RPUSH`es, 36 MB, fit in the sockets' buffers, since
Shoal's receive buffer had grown while it read the first million fast,
so the client finished sending before it read. And the solution failed
it, with Shoal running as the tests run it, with `-m1G` on two shards:
each reply waiting in the queue is counted as about 90 bytes, of which
80 are the queue's entry and only a few the reply's text, so a million of
them used up the shard's reply memory, 64 MB, and Shoal stopped reading.

## Exercise 3

The benchmark of the TLS section, with Shoal version 9 started once
with `--crypto-provider gnutls`, the default, and once with
`--crypto-provider openssl` (millions of requests a second, the best of
three runs of each):

| Workload | GnuTLS | OpenSSL |
|---|--:|--:|
| `SET`, random keys | 1.42 | 1.40 |
| `GET`, random keys | 1.39 | 1.40 |
| `SET`, one key | 4.13 | 4.16 |
| `GET`, one key | 4.36 | 4.39 |

They're the same, within 1%. (These runs, on another night, are a
little slower than the chapter's table with one key; compare the
columns of one table, not numbers from two.) `perf record`, during the
benchmark with random keys, shows why:

```
GnuTLS:   54.48%  std::_Hashtable<...>::_M_find_before_node
          11.52%  __memcmp_avx2_movbe
           ...
           0.98%  _aesni_ctr32_ghash_6x
OpenSSL:  53.99%  std::_Hashtable<...>::_M_find_before_node
          10.53%  __memcmp_avx2_movbe
           ...
           0.76%  _aesni_ctr32_ghash_6x
```

Both spend most of their time looking up keys in Shoal's hash table, as
Shoal without TLS does, and about 1% encrypting and decrypting, in a
function of the same name in both. It is the same code: AES-GCM in
assembly, with the processor's AES instructions, written by OpenSSL's
developers, which GnuTLS includes too (both libraries carry the string
"AES-NI GCM module for x86_64, CRYPTOGAMS by appro@openssl.org"). All
of the TLS library's code together, with `perf report --sort dso`, was
2.4% of the time with GnuTLS, and 3.4% with OpenSSL (`libcrypto` and
`libssl`): too little for the choice of library to matter here. A
workload with large values would encrypt more bytes for each request,
but still in the same code, in both.

## Exercise 4

The solution, in the same copy of version 9, remembers which listener
accepted each connection, and if it's the TLS one, asks for the client's
certificate:

```{.cpp include="examples/solutions/shoal-networking.patch" file="main.cc" snippet="client-dn"}
```

`tls::get_dn_information()` waits for the TLS handshake, which otherwise
happens on the first read or write, and returns the subject and the
issuer of the client's certificate, as *distinguished names*. With a
certificate authority, a server certificate and a client certificate
made by `test_v9.py`'s `make_certificate()`, and Shoal started with
`--tls-auth-clients yes`, a `valkey-cli --tls` with the client
certificate, and then one without it, got:

```
PONG
Error: Server closed the connection
```

And Shoal logged (shortened):

```
INFO  shoal - TLS client CN=localhost,O=alice, issued by CN=localhost,O=ca
WARN  shoal - connection with 127.0.0.1:55070 failed: std::system_error (error GnuTLS:-112, Certificate is required.)
```

## Exercise 5

With a server that ignores every other datagram, `udp.cc`'s client sends
`hello`, which the server ignores, and waits for an answer forever: UDP
never sends anything again by itself. The solution's client sends its
datagram again every 100 milliseconds, with a periodic timer, until the
answer comes:

```{.cpp include="examples/solutions/networking-ex5.cc"}
```

Its output:

```
server: lost hello
sent hello, received HELLO from 127.0.0.1:35371
server: lost datagram
sent datagram, received DATAGRAM from 127.0.0.1:35371
```

The timer's function can't wait for its `send()`, so the sends run in the
background, and a gate ([chapter](#ch:clean-shutdown)) waits for them
before the channel is closed.

Yes, an answer can come twice: if the first datagram wasn't lost but
only slow, the server answers both copies, and the second answer arrives
when the client already moved on, and is waiting for the answer to its
next word. The client above would take it for that answer. A real
protocol puts an identifier in each request, which the server copies
into its answer, so that the client can match answers to requests, and
ignore an answer it no longer waits for. DNS does exactly that: every
query carries a 16-bit identifier.
