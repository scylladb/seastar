# The case for Seastar {#sol:case-for-seastar}

## Exercise 1

On the laptop that this book's measurements use, an Intel Core Ultra 7
258V, `nproc` says 8, and `lscpu -e` shows that the eight cores aren't
alike:

```
CPU NODE SOCKET CORE L1d:L1i:L2:L3 ONLINE    MAXMHZ   MINMHZ       MHZ
  0    0      0    0 0:0:0:0          yes 4700.0000 400.0000 1490.6710
  1    0      0    1 4:4:1:0          yes 4800.0000 400.0000 4580.3130
  2    0      0    2 8:8:2:0          yes 4700.0000 400.0000 2200.1160
  3    0      0    3 12:12:3:0        yes 4800.0000 400.0000  504.2980
  4    0      0    4 64:64:8          yes 3700.0000 400.0000 3700.8311
  5    0      0    5 66:66:8          yes 3700.0000 400.0000 3701.4009
  6    0      0    6 68:68:8          yes 3700.0000 400.0000 3700.7490
  7    0      0    7 70:70:8          yes 3700.0000 400.0000 2875.6760
```

CPUs 0 to 3 are fast cores, which run at up to 4.7 or 4.8 GHz, each with
an L2 cache of its own, and an L3 cache that they share. CPUs 4 to 7 are
slower, more power-efficient cores, which run at up to 3.7 GHz, share one
L2 cache, and have no L3. There's one socket, and one NUMA node, so all
the memory is equally close to every core; on a server with two sockets,
`lscpu -e` would show two nodes.

For a design in which every shard does the same work, mixed cores mean
that the shards don't finish at the same time: the same computation took
0.98 seconds on a fast core of this laptop, and 1.37 on a slow one. A job
divided equally among all eight cores is done only when the slow shards
are, and work that waits for an answer from every shard waits for the
slowest. So on such a machine, a Seastar program is often better off on
the fast cores alone, chosen with `--cpuset`, as this book's measurements
do, even though that leaves cores unused.
[Chapter](#ch:deploying-and-tuning) discusses the choice; on servers,
whose cores are alike, the question doesn't arise.

## Exercise 2

Here is the program. It keeps the counters in four kinds of variables,
and each thread runs the same loop, which increments the counter of the
variant chosen on the command line:

```{.cpp include="examples/solutions/case-for-seastar-ex2.cc"}
```

Every variant runs the same loop, comparing the variant's name on each
step, so the times differ only by what the increment costs. Built with
`g++ -O2`, on the laptop of exercise 1, in three runs:

| Counters | 1 thread | 8 threads |
|---|---|---|
| one shared `std::atomic<long>` | 4.3–4.5 ns | 162–181 ns |
| one `std::atomic<long>` each, in its own cache line | 4.2–4.4 ns | 5.7 ns |
| one `std::atomic<long>` each, next to each other | 4.4 ns | 167–183 ns |
| one plain `long` each, next to each other | 1.2–1.5 ns | 4.9–5.6 ns |

(the time of an increment on each thread: each thread's hundred million
increments, divided into the time until all the threads were done).

With one thread, the three atomic variants cost the same, about four
nanoseconds an increment: an atomic increment is a single instruction,
which is slower than an ordinary one, but not much, when no other core
uses its cache line. With all eight cores, the shared counter costs about
forty times as much: before a core can increment it, the cache line that
holds it must come from the core that incremented it last, and the
cores' increments go one at a time, as if under a lock. This is the
cache-line bouncing that the chapter described.

Counters of their own, each in its own cache line, cost the same on eight
threads as on one: no core ever needs another's cache line, so the cores
don't slow each other down at all. That's the whole idea of shards, in a
dozen lines.

Counters of their own that sit next to each other are almost as slow as
the shared one. Eight `std::atomic<long>` take 64 bytes, one cache line,
so the threads fight over that line exactly as they did over the shared
counter, though no two of them touch the same variable. That's false
sharing: nothing in the program says that the counters are shared, and
only their addresses reveal it.

Plain `long` counters, next to each other, are faster than the atomic
ones on one thread, because a plain increment is cheaper than an atomic
one. With eight threads, they slow down too, but only about four times,
not forty. The compiled loop does write every increment to memory (the
comparisons in the loop keep the compiler from keeping the counter in a
register), so the cache line bounces between the cores as before. But
an ordinary write doesn't wait for the cache line: the core puts it in
its store buffer, and goes on to the next step, and the writes reach the
cache line when it arrives. An atomic increment must have the cache line
before it can finish, so it waits for every trip of the line. Two things make plain counters a
trap, though. A plain counter that several threads share is a data race:
increments are lost, and the program's behavior is undefined. And in a
loop that does nothing but increment a plain counter, the compiler may
replace the whole loop by a single addition, since no other thread is
supposed to see the counter in the middle; here, the comparison of the
variant's name in the loop happens to prevent that, but in a simpler
program, the "measurement" would be zero. Measuring something that the
compiler can remove is a trap that we meet again in
[chapter](#ch:object-lifetime)'s exercise 3.

## Exercise 3

On the laptop, Valkey 9.0.6, with `valkey-benchmark -t get -P 16
--threads 4 -n 100000000 -q`, served about 2.2 to 2.6 million `GET`s a
second, with a median latency of about 0.27 ms. In `top`, with `H`, only
one of `valkey-server`'s threads was busy, at about 80 to 85% of a core;
its other threads, the background ones that Valkey calls `bio_*`, were
idle. So Valkey used one core, and one core is what limits it. The
client wasn't the limit: with one benchmark thread, which used about half
a core, Valkey served about as many `GET`s a second as with four.

With `-P 64 -c 200`, which keeps sixteen times as many requests in
flight (four times as many on each of four times as many connections,
since the default is 50), the throughput rose to about 3.5 million
`GET`s a second, with the server's thread at 92% of a core. The median
latency rose much more, to about 3.3 ms, ten times as much: the server
doesn't work faster, it just has more requests waiting for it, and each
request waits behind more of them. A server running at the limit of one
core can trade latency for throughput only this way, by batching; to do
more work, it needs more cores, which is what Seastar's shards provide.

## Exercise 4

Eight cores at 2.5 GHz execute 20 billion cycles a second, together. Ten
million requests a second leave each request 2,000 cycles, which is 800
nanoseconds on one core: each core must serve one request every 800
nanoseconds, 1.25 million a second.

A cache miss that goes to main memory takes about 80 to 100 nanoseconds,
200 to 250 cycles at 2.5 GHz. So a request could afford about eight to
ten cache misses, if it did nothing else. But a request must also be
read from the network, parsed, executed and answered, so a realistic
budget is a few misses, and each request's data must mostly be in the
cache already. (A modern processor can wait for several misses at once,
if the program's next addresses don't depend on the data being fetched,
so independent misses cost less; a chain of pointers, where each miss
reveals the next address, costs the full price for each.)

The same arithmetic rules out much more than cache misses. A context
switch costs a few microseconds, several times the whole budget; so does
a contended lock, or a system call for every request. This is why a
server that aims at such rates has no thread switches, no locks, and
reads many requests with each system call: the design that the chapter
described.
