# Concurrency within a shard {#sol:concurrency-within-a-shard}

## Exercise 1

The patch to Shoal version 4, `examples/solutions/shoal-naive-write.patch`,
writes directly to the stream, as the first attempt did:

```{.cpp include="examples/solutions/shoal-naive-write.patch" file="pubsub.cc" snippet="write"}
```

Built against a `release` Seastar, it passes all 38 of version 4's
tests. None of them makes two writes to one connection overlap: that
takes a subscriber whose buffers are full, so that one write waits, and
another writer, a publisher or the subscriber's own replies, starting a
write in the meantime. This program makes it happen: two publishers send
100 KB messages to one subscriber, which reads slowly, through a small
receive buffer, and sends `PING`s of its own:

```{.python include="examples/solutions/concurrency-within-a-shard-ex1.py"}
```

The patched Shoal, built against a `debug` Seastar, with `CMAKE_PREFIX_PATH`
naming the `debug` build's installation, and started with
`--pubsub-delivery parallel` (or `sequential`, which also writes with
`write()`), stopped within a second, in five runs out of five. First,
Seastar's network code complained, about eighteen times:

```
ERROR ... seastar - wrapped_iovecs are re-populated live, at: ...
```

and then AddressSanitizer stopped the program:

```
==96766==ERROR: AddressSanitizer: heap-use-after-free on address ...
    #0 ... in std::vector<iovec, std::allocator<iovec> >::clear() ...
    #1 ... in operator() src/net/posix-stack.cc:108
```

with an exit status of 1. Against a `release` Seastar, the same copy
logged the same complaint about 190 times, and the subscriber's
connection failed, but Shoal went on running.

The tests pass because a test checks only what it makes happen, and
overlapping writes need a particular timing, which ordinary tests don't
create. The bug is there all the same, and the stress program finds it
at once. Seastar's network code noticed that a write started while the
previous one was still in flight, and complained, which shows that the
two writes shared the stream's state; and in `debug` mode,
AddressSanitizer then found them using memory that one of them had
freed. In a `release` build, the second finding is invisible: freed
memory is reused without a check, and the program goes on with
corrupted data, until it crashes, or sends a client garbage, somewhere
else. That's the pattern of most concurrency bugs in Seastar programs,
and the reason to run tests, and stress tests especially, in `debug`
mode, as [chapter](#ch:testing) does.

## Exercise 2

The solution is a patch to Shoal version 4,
`examples/solutions/shoal-pubsub-patterns.patch`. Its tests came first, and
pass against Valkey:

```{.python include="examples/solutions/shoal-pubsub-patterns.patch" file="test_solutions.py"}
```

Writing them answered the exercise's questions about Valkey. A pattern
subscriber receives a `pmessage`, an array of four: the word `pmessage`,
the pattern, the channel, and the message. `PSUBSCRIBE` confirms each
pattern like `SUBSCRIBE` confirms a channel, and the count in every
confirmation is of the channels and the patterns together. And `PUBLISH`
counts every message it sends: a client subscribed to a channel and to a
pattern that matches it receives the message twice, once as a `message`
and once as a `pmessage`, and `PUBLISH` counts it twice.

`glob_match()`, which [chapter](#ch:how-seastar-runs-your-code) wrote for
`KEYS`, is in an anonymous namespace in `commands.cc`; the patch moves it
out, and declares it in `commands.hh`:

```cpp
// Checks if s matches a glob-style pattern, as KEYS and PSUBSCRIBE do.
bool glob_match(std::string_view pattern, std::string_view s);
```

A client keeps its patterns next to its channels, and counts them
together, as Redis does:

```{.cpp include="examples/solutions/shoal-pubsub-patterns.patch" file="pubsub.hh" snippet="client"}
```

and the class `pubsub` gets a second table, `_patterns`, from each
pattern to its subscribers. `PSUBSCRIBE` and `PUNSUBSCRIBE` are
`SUBSCRIBE` and `UNSUBSCRIBE` for patterns:

```{.cpp include="examples/solutions/shoal-pubsub-patterns.patch" file="pubsub.cc" snippet="psubscribe"}
```

with the reply that a pattern's subscriber receives:

```{.cpp include="examples/solutions/shoal-pubsub-patterns.patch" file="pubsub.cc" snippet="pmessage"}
```

`PUBLISH` changes most. It used to send one message to a list of
subscribers; now it sends a message to the channel's subscribers, and a
`pmessage` to each subscriber of each pattern that matches the channel,
so it makes a list of recipients, each with what to send it:

```{.cpp include="examples/solutions/shoal-pubsub-patterns.patch" file="pubsub.cc" snippet="publish"}
```

Every other change follows from "subscribed" now meaning "subscribed to
a channel or a pattern": `pubsub::execute()` and the connection's loop
ask `subscribed()` instead of checking for channels, `forget()` removes a
client's patterns too, and the "still subscribed" check before each
write, in the first and the third ways of delivering, checks the table
that the recipient came from. The background loop needs one more check,
which is easy to miss: a client can be a recipient twice, through its
channel and a pattern, so after the per-subscriber limit disconnects it
for the first, the loop must skip it for the second, instead of sending
it a message after `forget()`.

With these, the 5 new tests pass against Valkey and the patched Shoal, and so do
all 43 tests with the default, background delivery. With
`--pubsub-delivery sequential` or `parallel`, 42 pass: all but
`test_slow_subscriber`, which the two ways that wait fail by design, as
the chapter showed.

## Exercise 3

The patch `examples/solutions/shoal-limited-delivery.patch` changes only
`delivery::parallel`:

```{.cpp include="examples/solutions/shoal-limited-delivery.patch" file="pubsub.cc" snippet="parallel"}
```

With `--pubsub-delivery parallel`, 5,000 subscribers, and 2,000 messages
of 1,000 bytes, measured in two runs each, which agreed:

| | Delivery | Peak memory of Shoal |
|---|---|---|
| `coroutine::parallel_for_each` | 552,000 messages a second, in 18.1 s | 181 MB |
| `max_concurrent_for_each(..., 100, ...)` | 175,000 messages a second, in 57.1–57.2 s | 140 MB |

(the peak resident memory, `VmHWM` in `/proc`, 43 MB when Shoal
started). The limit makes each `PUBLISH` more than three times slower,
and saves about 40 MB of memory.

Both come from the same cause. Without the limit, a `PUBLISH` starts all
5,000 writes at once, and each holds its own copy of the message, with
its coroutine frame, until it's written: 5,000 copies of a little more
than a kilobyte, several megabytes for each `PUBLISH`, while they wait
for the subscribers' sockets. With the limit, only 100 are waiting at a
time, and the next starts only when one of them has finished. But
finishing a write means the subscriber's connection has taken the
message, and the subscribers read at their own pace, so the hundred
slots are mostly waiting for slow subscribers, while the other 4,900
writes, which could have proceeded, wait for a slot. The limit turns
the delivery into a hundred lanes, each as slow as its slowest
subscriber. (Most of the 140 MB is the 5,000 connections themselves,
with their buffers, which the limit doesn't change.)

So a limit is the right tool where too much at once hurts something,
as with `pubsub-bench`'s connections and the server's backlog, and the
wrong one where the only cost is memory, which a little of goes a long
way here. If memory were the concern, the better cure is the one of
[chapter](#ch:coordination)'s Shoal version 6: a queue for each
subscriber, which keeps a waiting message without a frame of its own, and
a limit on the total memory of all the waiting messages, rather than on
the number of writes.

## Exercise 4

This script runs Shoal version 4 on one shard, and `pubsub-bench` with
1,000 subscribers, and measures the time from starting `pubsub-bench`
until it ends, which is mostly the time it takes to connect, since it
then publishes only one message. It reads `TcpExtListenOverflows` before
and after each run, and in the runs where Shoal is frozen, it shows what
`ss -ltn` showed while Shoal was frozen:

```{.bash include="examples/solutions/concurrency-within-a-shard-ex4.sh"}
```

With the environment variables `SHOAL` and `BENCH` naming the two
programs, two runs of the script, and three more runs of the last case,
gave:

| `--max-connecting` | Shoal | Time | Overflows |
|---|---|---|---|
| 1 | idle | 0.58–0.59 s | 0 |
| 50 | idle | 0.14–0.16 s | 0 |
| 1000 | idle | 1.42–1.62 s | 654–679 |
| 50 | frozen for 0.33 s | 0.47 s | 0 |
| 1000 | frozen for 0.33 s | 1.60–8.03 s | 2,190–4,012 |

With one connection at a time, each subscriber takes about 0.6
milliseconds, for two round trips that nothing overlaps: the handshake of
its connection, and its `SUBSCRIBE` with the reply. With 50 at a time,
the round trips overlap, and connecting is four times faster. But with
all 1,000 at once, it's ten times *slower* than with 50, and the reason
is in the last column: even an idle Shoal overflowed its backlog, in
every run. `pubsub-bench` starts the 1,000 connections at once, and the
kernel completes their handshakes, and queues them, much faster than
Shoal's one shard accepts them and starts their fibers. The backlog,
`Send-Q` in `ss -ltn`, is 100, so about two thirds of the connections
were dropped, and retried a second later, which is where most of the 1.4
to 1.6 seconds went.

The frozen Shoal shows the backlog itself. While it was frozen, its
kernel went on completing handshakes, and queued the connections for an
`accept()` that didn't come. With 50 at a time, `Recv-Q` showed 50
waiting, which fit in the backlog of 100; when Shoal resumed, it
accepted them all, and the whole run took the third of a second that
Shoal was frozen, plus the 0.14 seconds that it takes anyway. With all
1,000 at once, `Recv-Q` showed 101, a full backlog (Linux allows one more
than the backlog's length), and every other connection was dropped,
which is why there were more overflows than connections: a connection
retried while the backlog was still full overflowed again, and counted
again.

That's also why the run takes so much longer than the third of a second:
after Shoal resumes, the dropped connections don't come back until their
client's kernel retries them, a second after the first attempt, the
earliest. Then about 900 retry within a moment of each other, the same
burst that overflowed even an idle Shoal, so many are dropped again; and
each time a connection is dropped again, its kernel waits twice as long
before the next retry. A connection dropped three times waits 1 + 2 + 4
= 7 seconds, which is the 8 seconds of the slowest runs. (Not all the
drops are of the client's first packet: a connection whose handshake
the client considers complete, while Shoal's kernel dropped its last
packet, is retried on other timers, and the runs ended at various times
between these.)

So, unlike most work, more concurrency here doesn't just stop helping
at some point: it makes things much worse, because the cost of too much
is a timeout of a whole second, much larger than the work. A limit
smaller than the server's backlog, like `--max-connecting`'s default of
10, or the 50 here, never overflows it, even when the server is slow to
accept, and gets most of the benefit of the concurrency.

## Exercise 5

With about 950,000 keys, loaded with `valkey-benchmark -t set -r
10000000 -n 1000000 -P 16`, Shoal version 4 on one shard: `FLUSHALL SYNC`
took 0.08 seconds, and `FLUSHALL ASYNC` replied in 5 milliseconds. `SET`s
sent right after the `FLUSHALL ASYNC`, by the same benchmark, ran at 1.44
million a second, against 1.59 million while the keys were being loaded,
and 1.27 million a minute later, with nothing in the background. The
differences between these runs are larger than any effect of the
background destruction: neither visibly slows the other.

The hint explains why. The destruction takes about as long as `FLUSHALL
SYNC`, a tenth of a second of work, which the shard divides with the
`SET`s, a task quota at a time: while there are `SET`s to execute, the
destruction gets only some of the shard's time, and each `SET` waits at
most a quota for its turn. So the `SET`s lose a little throughput, a
fraction of a tenth of a second, during the tenth of a second after the
`FLUSHALL`, which a benchmark of a second or more can't see; and the
destruction takes a little longer than it would alone, which nobody
waits for.

A second `FLUSHALL ASYNC` before the first one has finished starts a
second destruction, of whatever was stored since the first, while the
first still runs: both old stores are in memory until their destructions
end. One more is harmless. But a client that loads data and sends
`FLUSHALL ASYNC` again and again, faster than the destructions finish,
makes the old stores pile up in memory without limit, since nothing
waits for them, or limits how many run at once. That's the danger of
background work that the chapter described in "What waits for it", and
the reason [chapter](#ch:coordination) has ways to limit such work.
