# Coroutines {#sol:coroutines}

The solutions of exercises 1 and 2 are a patch to Shoal version 1,
`examples/solutions/shoal-coroutines.patch`, which adds `INCR` and `APPEND`,
and their tests; the rest is version 1 as the chapter showed
it.

## Exercise 1

`INCR` is one more function in `commands.cc`:

```{.cpp include="examples/solutions/shoal-coroutines.patch" file="commands.cc" snippet="incr"}
```

and one more line in the table of commands, `{"incr", {2, incr}}`: the
command and its key, two words. It looks the key up with `find()`, rather
than with `db[key]`, which would insert an empty value for a key that
doesn't exist: an empty value is an error, as for any string that isn't
a number, but a missing key counts as 0. `std::from_chars()` parses the
number, and must use all of the string: Redis refuses `"1 "` and `" 1"`
as well as `"abc"`. It also refuses a number that doesn't fit in 64 bits,
which `from_chars()` reports as an error too, and an increment past the
largest 64-bit integer, with an error of its own.

The tests check each of these cases:

```{.python include="examples/solutions/shoal-coroutines.patch" file="test_solutions.py"}
```

They pass against `valkey-server`, which shows that they test what Redis
does, and then against the patched Shoal: with the 15 tests of version 1
and the 10 here (the parametrized test counts once for each value), 25
tests in all.

As the exercise warns, `r.incr(key)` in the `redis` library doesn't send
`INCR`: it sends `INCRBY key 1`, a more general command, which Shoal
doesn't have, so a test written with it fails against Shoal with "unknown
command 'INCRBY'", though it passes against Valkey. The test then tests
the library's choice of command, not ours, which is why the tests send
`INCR` themselves, with `execute_command()`.

## Exercise 2

`APPEND` is short:

```{.cpp include="examples/solutions/shoal-coroutines.patch" file="commands.cc" snippet="append"}
```

This time, `db[req[1]]` is what we want: a missing key gets an empty
value, to which the bytes are appended. This program appends a byte to the
same key, again and again, sending a thousand `APPEND`s in each
pipeline:

```{.python include="examples/solutions/coroutines-ex2.py"}
```

With the server on CPU 0 and the client on CPU 1, in three runs:

| Appends | `valkey-server` | Shoal |
|---|---|---|
| 100,000 | 0.25–0.27 s | 0.29 s |
| 1,000,000 | 2.58–2.69 s | 9.18–9.27 s |

Valkey's time grows in proportion to the number of appends: ten times as
many take ten times as long. Shoal's grows faster: ten times as many take
32 times as long, and with a million appends, Shoal is three and a half
times slower than Valkey. The two parts of Shoal's time grow differently.
Part of it is the same work that Valkey does, sending and parsing the
requests, about a quarter of a second for a hundred thousand, which
grows in proportion. The rest is Shoal's extra work, the difference from
Valkey: about 0.03 seconds for a hundred thousand appends, and about 6.6
for a million, two hundred times as much for ten times as many. That
part grows with the square of the number of appends, or a little faster,
since a long value also fits the processor's caches less well.

The hint explains why. `sstring::append()` allocates a new string, of
the old length plus the new, copies both into it, and frees the old one.
So the millionth append copies a million bytes, and a million appends
copy, together, about half a million million bytes: the cost of each
append grows with the length of the value. A Redis string, on the other
hand, keeps room to grow: when an append doesn't fit, Redis allocates
more than it needs, about twice as much for a small string, so most
appends copy only the new bytes, and the occasional growth costs, on
average, a constant amount per byte. `std::string` does the same, and
`sstring` doesn't, because it's designed for strings that are built once
and then passed around, not grown a byte at a time. A Shoal that wanted
fast `APPEND`s would store its values in a type that reserves room, or
keep a value as a list of pieces.

## Exercise 3

With the patched Shoal on CPU 0, `valkey-benchmark -t set -n 160000` on
CPUs 1 to 3, and `strace -f -c -e trace=sendmsg -p` attached to Shoal
during each run:

| Pipeline | `sendmsg` calls | `SET`s answered by each call |
|---|---|---|
| `-P 1` | 160,001 | 1 |
| `-P 16` | 10,092 | about 16 |

(`strace` slows Shoal down a lot, to 58,000 `SET`s a second with `-P 1`,
so only the counts mean anything here.) Without pipelining, each call
sends one reply, as one would expect from a server that flushes after
every reply. With pipelines of 16, each call sends about 16 replies,
though Shoal still calls `flush()` after each of them.

The reason is that `flush()` on a socket's stream doesn't send anything
by itself. Seastar's `connected_socket::output()` makes streams that batch
their flushes: `flush()` only marks the stream as needing a flush, and
the reactor flushes the marked streams when it next polls, between
tasks. With `-P 1`, a client sends its next request only after the reply
to the previous one, so there is never more than one reply waiting: one
call per reply. With `-P 16`, a single read brings in up to 16 requests,
which Shoal executes one after another without waiting, each `flush()`
only marking the stream; by the time the reactor gets to flushing, all
16 replies are in the stream's buffer, and go out in one call. So the
program can be written simply, flushing after every reply, and still
send as few system calls as the client's pipelining allows. A system
call costs a microsecond or so, which is why this matters: with `-P 16`,
Shoal makes a sixteenth of the calls.

## Exercise 4

With Shoal started with `-c2 -m1G`, a request that announces a string of
600 megabytes:

```
$ printf '*2\r\n$3\r\nGET\r\n$600000000\r\n' | nc 127.0.0.1 6379
-ERR Protocol error: invalid bulk length
```

gets an error at once, because Redis limits a string to 512 megabytes,
and Shoal checks the announced length against that limit before
anything else. A request that announces 500 megabytes, and then sends
nothing, gets no reply, and Shoal logs

```
WARN  ... shoal - connection with 127.0.0.1:58376 failed: std::bad_alloc (std::bad_alloc)
```

The chapter's `read_exactly()` allocates a string of the announced
length before reading any of it, so as to read the data into it. With
`-c2 -m1G`, each shard has 512 megabytes, and a single allocation of 500
can't succeed in it, so the allocation throws `std::bad_alloc`, which
ends this connection's coroutine, and Shoal logs it and drops the
connection. Everything else goes on: a `PING` right afterwards gets its
`PONG`.

Shoal survived, but only by luck. With a little less announced, say 200
megabytes, the allocation would succeed, and the client, having sent 30
bytes, would hold 200 megabytes of Shoal's memory for as long as it
cared to keep the connection open; a few such clients, or one on each
shard, and Shoal runs out of memory for everyone. A server must never
trust what a client announces: it should allocate memory as the data
actually arrives, read a large value in pieces, and limit how much
memory all the requests being read may use together, which is what
Shoal's version 6, in [chapter](#ch:coordination), does.

## Exercise 5

Here is the program. `parse_ready()` is `parse_returning()` without the
coroutine: it returns a future that is already available, or already
failed, instead of a coroutine's future. A loop calls each of them a
million times, and checks each result with `coroutine::as_future()`, as
the chapter's `count_errors_checking()` did:

```{.cpp include="examples/solutions/coroutines-ex5.cc"}
```

Pinned to CPU 0, it printed the same in each of three runs, give or take
a nanosecond:

```
coroutine, success              10 ns each (0 errors)
ordinary function, success       4 ns each (0 errors)
coroutine, failure              47 ns each (1000000 errors)
ordinary function, failure      39 ns each (1000000 errors)
```

On success, the ordinary function is two and a half times as fast: 4
nanoseconds instead of 10. What it saves is the coroutine's frame.
Calling a coroutine allocates a frame for its local variables and its
state, and destroys it when the coroutine ends, even when, as here, the
coroutine never waits; an ordinary function keeps its variables on the
stack, and its future holds the result directly. About 6 nanoseconds,
then, is what a coroutine's frame costs: an allocation, a free, and the
bookkeeping around them.

On failure, both are slower, and the difference is the same 6 or 8
nanoseconds, a smaller part of the whole. Most of a failure's 40
nanoseconds is the exception itself: `make_exception_future()`, like
`return_exception()`, makes a `std::exception_ptr`, which allocates the
exception object, and the caller's check examines it and frees it. A frame
is cheap next to that.

So a frame costs a few nanoseconds: nothing for code that waits, since a
wait costs far more, and noticeable only in a function that is called
millions of times and usually finishes without waiting. For such a
function, an ordinary function that returns a ready future is the faster
form, which is what [chapter](#ch:futures-and-promises) does with Shoal's
request reader: it reads a request without a coroutine when the request
is already in its buffer.

## Exercise 6

This program sends a million `APPEND`s in one pipeline, which `redis`'s
`execute()` sends in full before it reads any reply, and gives up if the
socket makes no progress for twenty seconds:

```{.python include="examples/solutions/coroutines-ex6.py"}
```

Against `valkey-server`, it's done in 2.47 seconds. Against the patched
Shoal, it's stuck, and gives up after twenty seconds. While it was stuck,
`ss -tn` showed both directions of the connection full:

```
State Recv-Q Send-Q  Local Address:Port  Peer Address:Port
ESTAB 124758 2758089     127.0.0.1:39754    127.0.0.1:6486
ESTAB 326740 2625536     127.0.0.1:6486     127.0.0.1:39754
```

The first line is the client's end: 2.8 megabytes of requests it still
has to send, and 125 kilobytes of replies that have arrived but that it
hasn't read. The second line is Shoal's end: 327 kilobytes of requests
that have arrived but that it hasn't read, and 2.6 megabytes of replies
waiting to be sent. Nothing moves, because each side waits for the other.
Shoal executes a request, and writes its reply; when the socket's buffers
are full, `out.write()` waits until the client reads some replies, and
Shoal reads no more requests until it does. The client reads replies
only after it has sent all its requests, and it can't send more, since
Shoal doesn't read them. That's a deadlock: each waits for the other to
read.

Valkey finishes because it never stops reading. When it can't send a
reply, it keeps it in memory, in the client's output buffer, and goes on
reading and executing requests; for an ordinary client, that buffer has
no limit. So Valkey buffered most of a million replies, a few megabytes,
until the client got to reading them.

Each choice has a cost. Valkey serves a client like this one, but gives
every client the power to make it buffer without limit, which a
malicious or broken client can use to exhaust its memory. Shoal never
holds more than a socket buffer of replies for a client, so its memory
is bounded, and a client that doesn't read slows only itself, but it
can't serve a client that sends a huge pipeline before reading. Real
clients avoid the problem either way: they send pipelines in batches of
reasonable size, reading each batch's replies before sending the next,
as Redis's documentation recommends, or read while they write, as
`valkey-benchmark` does, with a window of requests in flight.
[Chapter](#ch:networking) returns to this trade-off.
