# Deploying and tuning {#sol:deploying-and-tuning}

## Exercise 1

The program, `examples/solutions/deploying-and-tuning-ex1.cc`, counts
primes in an idle handler, which asks the reactor whether it has work
every `check` numbers, and returns as soon as it does:

```{.cpp include="examples/solutions/deploying-and-tuning-ex1.cc" snippet="handler"}
```

It never says `no_more_work`, since there are always more numbers to
try, so while it runs, the reactor never sleeps. A timer that should
fire every millisecond notes how late it comes, for a second without the
handler, then with it, checking every 100 numbers, and every million;
run with `--reactor-backend linux-aio`:

```
no idle handler: heartbeat up to 0.69 ms late
checking every       100 numbers:   265696 primes found, heartbeat up to 0.06 ms late
checking every   1000000 numbers:   283146 primes found, heartbeat up to 357.91 ms late
```

Checking every 100 numbers, the handler gives the CPU back at once, and
the timer comes on time, even more so than without the handler, since
the reactor, kept busy, never sleeps, and doesn't need to wake up for
the timer. Checking every million, it holds the CPU for a third of a
second at a time, and the timer waits as long, for 6% more primes. The
handler's work must come in small steps, as a task's must
([chapter](#ch:stalls)).

With the default backend, `io_uring`, the program hangs, at the first
step with the handler: a bug in Seastar, which had been found before,
with DPDK, as issue <https://github.com/scylladb/seastar/issues/1890>,
and whose fix, <https://github.com/scylladb/seastar/pull/3553>, wasn't
merged yet when we wrote this; with it, the program works on `io_uring`
too. With `io_uring`, the
reactor notices that a timer expired either while it runs tasks, or when
it sleeps, but not while it runs an idle handler and doesn't sleep; so
the timer never fires, and the handler, which waits for the reactor to
have work, waits forever. A smaller program shows the same with
`--poll-mode`, in which the reactor never sleeps either: a timer that
ticks every millisecond, while a coroutine sleeps for 100 milliseconds,
on `io_uring`, and the sleep never ends; on `linux-aio` and `epoll`, it
takes 100 milliseconds.

## Exercise 2

The program, `examples/solutions/deploying-and-tuning-ex2.cc`, reads
the reactor's counters before and after:

```{.cpp include="examples/solutions/deploying-and-tuning-ex2.cc" snippet="stats"}
```

On a core of its own, and then under a quota of half a core:

| | Alone | Quota of half a core |
|---|---:|---:|
| Uptime | 2,000 ms | 2,001 ms |
| Busy | 992 ms | 999 ms |
| Idle | 1,008 ms | 1,002 ms |
| Awake | 1,200 ms | 1,321 ms |
| CPU | 1,199 ms | 1,005 ms |
| Steal | 0.7 ms | 316 ms |
| Polls | 2,101,616 | 1,516,855 |
| Tasks | 1,983 | 1,669 |

Alone, the shard is busy half of the time, as the program does, and
awake longer, 1,200 ms, since after its work it polls for a while before
it sleeps ([chapter](#ch:how-seastar-runs-your-code)); and its CPU time
is its awake time. Under the quota, its CPU time is held to half of the
two seconds, while it's awake longer, and the difference, 316 ms in
which the shard was ready to run but the kernel didn't let it, is
counted as steal time. So steal time, which on a cloud machine is the
hypervisor's, also shows a quota, and a server that reports steal time
on a machine of its own is being throttled.

## Exercise 3

Under the quota of two cores, with `--cpuset 0-3`, and `--smp 2`, Shoal
runs two shards, which the quota can feed, in thousands of requests a
second, and the latency of `GET`s without pipelining, in milliseconds,
the best of three runs:

| Shards | `SET` | `GET` | Median | 99th percentile | Worst |
|---|---:|---:|---:|---:|---:|
| 4 (the default) | 1,570 | 1,570 | 0.14 | 0.29 | 47 |
| 2 (`--smp 2`) | 4,346 | 4,647 | 0.047 | 0.071 | 0.30 |

Two shards serve almost three times as many requests, almost as many as
two shards without a quota did in the chapter, and their worst latency
is a third of a millisecond, against 47. The quota still applies, but two
shards never use more than it allows, so the kernel never stops them.
