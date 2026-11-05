# A tutorial introduction {#sol:tutorial-introduction}

## Exercise 1

`hello` gets a sleep after its greeting, here of 30 seconds, or as many as
`--seconds` says:

```{.cpp include="examples/solutions/tutorial-introduction-ex1.cc"}
```

While it sleeps, `ps -L -p` with its process ID lists its threads. On the
laptop of [chapter](#ch:case-for-seastar), with its eight cores:

| Shards | Threads | Their names |
|---|---|---|
| `-c1` | 4 | `hello`, `syscall-0`, and two `hello-ust` |
| `-c2` | 6 | the same, and `reactor-1` and `syscall-1` |
| all 8 | 18 | `hello`, `reactor-1` to `reactor-7`, `syscall-0` to `syscall-7`, and two `hello-ust` |

Each shard has two threads: its reactor, which runs the shard's tasks,
and a thread named `syscall-N`, which makes the system calls that would
block the reactor if it made them itself, such as opening a file on a
file system that doesn't support asynchronous opening. Shard 0's reactor
is the program's main thread, which is why it has the program's own name;
the other shards' reactors are named `reactor-N`. So `-c1` has two threads
for its shard, `-c2` four, and eight shards, the default on this machine,
sixteen. The remaining two threads, both named `hello-ust`, belong to
LTTng, the tracing library that Seastar uses for its tracepoints
([chapter](#ch:debugging-and-profiling)): they're there in every run,
whatever the number of shards.

So a Seastar program has more threads than shards, but only one thread
per shard does the shard's work. The others mostly sleep, waiting for
the rare system call or trace event, and don't compete with the reactors
for the cores.

## Exercise 2

`count()` now says when it ends, measured from a starting time that the
caller gives it, and the program runs three counts at once, with
`coroutine::all`:

```{.cpp include="examples/solutions/tutorial-introduction-ex2.cc"}
```

With one shard, it prints the counts' numbers as they come, interleaved,
and

```
tack ends after 800ms on shard 0
tick ends after 900ms on shard 0
tock ends after 1000ms on shard 0
```

and with `-c4`, the same.

The three counts end when their last sleep does: `tick` after three
sleeps of 300 ms, 900 ms; `tock` after two of 500 ms, 1,000 ms; `tack`
after four of 200 ms, 800 ms. Together, they take as long as the longest
of them, 1,000 ms, not the 2,700 ms that they would take one after the
other.

With `-c4`, nothing changes, and every line still says shard 0. The three
counts are coroutines that the main function starts, on the shard where
it runs, shard 0; nothing moves them to other shards, and Seastar never
moves work between shards by itself. More shards would help only if the
program gave them work, as `shards.cc` did with `smp::invoke_on_all()`.
And here they wouldn't help anyway: the counts spend almost all of their
time asleep, not computing, so one shard runs all three at once with
room to spare. Concurrency comes from waiting at the same time, which one
shard does well; parallelism, from computing at the same time, which
needs more shards.

## Exercise 3

Here is the server. Its connection's coroutine keeps what arrived after
the last newline in a `std::string`, and writes a line back only when its
newline arrives:

```{.cpp include="examples/solutions/tutorial-introduction-ex3.cc"}
```

With the client of the exercise, `(printf hel; sleep 1; printf 'lo\n';
sleep 1) | nc localhost 1234`, the original server echoed `hel` at once,
and `lo` a second later; this one says nothing for a second, and then
echoes `hello` when the newline arrives.

A read returns whatever bytes have arrived, which needn't be a whole
line, or one line: it may be part of a line, as here, or several lines,
or the end of one line and the start of the next. That's why the inner
loop handles every newline in the buffer, and keeps only the rest: a
server that assumed one read per line would work with `nc` typing by
hand, and fail with a client that sends faster, or in pieces. TCP is a
stream of bytes, and any structure in it, lines or requests, is the
program's to find, as Shoal does in [chapter](#ch:coroutines).

The string grows without limit if a client sends a long line without a
newline, or never sends one. A real server would limit it, and refuse a
line that is too long; Shoal's version 6, in [chapter](#ch:coordination),
limits the memory of the requests it reads in the same spirit.

## Exercise 4

With the default load balancing, connections opened one after the other,
each closed before the next is opened, were all served by shard 0. Held
open together, four connections went to four different shards, two on
each shard when we opened eight. Seastar's default,
`connection_distribution`, gives each new connection to the shard that
has the fewest connections at that moment: with each connection closed
before the next arrives, that's always shard 0, which has none; with the
connections still open, the shards fill up in turn.

With the port algorithm, the server is `echo-all-cores` with one change,
the setting of `opts.lba` in `serve()`:

```{.cpp include="examples/solutions/tutorial-introduction-ex4.cc"}
```

The shard is the client's port modulo the number of shards. With
`nc -4 -p 50000 127.0.0.1 1234` and the ports after it, ports 50000 to
50003 went to shards 0 to 3, port 50005 to shard 1, and port 50010 to
shard 2, whatever else was connected. (On Fedora, `nc` is Ncat, which
binds an IPv6 address when it's given `-p`, and then ignores the port;
`-4` makes it use IPv4.) A client that knows the number of shards can
therefore choose which shard serves it, by choosing its port; that's
how ScyllaDB's "shard-aware" drivers connect each connection to the shard
that owns the data it asks for, as
[chapter](#ch:designing-a-sharded-application) explains. The default is
the right choice for clients that don't know about shards, which is
most of them.

## Exercise 5

With `pong` on CPU 0, and `valkey-benchmark` on CPUs 1 to 3, with 96
connections and 2 threads, as in the chapter:

| `-P` | Requests per second | Median latency (`p50`) | 96 × `P` ÷ `p50` |
|---|---|---|---|
| 1 | 345,000 | 0.279 ms | 344,000 |
| 4 | 1,387,000 | 0.271 ms | 1,417,000 |
| 16 | 5,594,000 | 0.263 ms | 5,840,000 |
| 128 | 13,017,000 | 0.463 ms | 26,540,000 |

(the best of three runs, each of about half a minute, with its median
latency).

Up to 16 requests per connection, the throughput grows almost in
proportion to the pipeline, while the median latency stays the same,
about a quarter of a millisecond. And dividing the requests in flight by
the median latency predicts the throughput to within ten percent. That's
Little's law: in any system in a steady state, the number of requests
in it equals the rate at which they arrive times the time each spends in
it. With the time per request fixed, by the round trip through the
client, the kernel's network stack and the server, the only way to serve
more requests a second is to have more of them in flight at once, which
is what pipelining does. With few requests in flight, `pong` is mostly
idle, waiting for the next request to arrive.

With 128, the prediction is twice the measurement, and that teaches
something about the benchmark. `valkey-benchmark` sends a whole batch, 128
requests on a connection, and then waits until all of their replies have
arrived before it sends the next batch; and it measures each request's
latency from when its batch was sent. With small batches, the round trip
dominates: all of a batch's replies come back at about the same time,
and the whole batch is in flight nearly all the time, as Little's law
assumed. But with 128 on each of 96 connections, `pong` is busy all the
time, and serves a batch's requests one after another, so their replies
come back spread out, over the time it takes to serve the batch. On
average, half of a batch is still waiting, and the other half has been
answered and waits for its batch-mates. So only about half as many
requests are really in flight, and the measured throughput is half the
prediction. This is also the point where the latency grows: requests now
wait for the server, rather than the server for them. A server is best
measured just there, at its limit, which is why this book's benchmarks
pipeline deeply.
