# Deploying and tuning {#ch:deploying-and-tuning}

A Seastar program assumes that it owns the machine: a core for each
shard, which nothing else uses, the memory, divided among the shards,
and the network card's interrupts handled where they don't get in the
way. Laptops, containers and cloud machines don't always give it that.
This chapter is about running a Seastar program well: choosing its
cores and its memory, tuning the machine around it, choosing a reactor
backend, building it for production, and the statistics with which the
reactor tells how it's doing.

## Cores {index="cores"}

Seastar runs a shard on every core it may use, unless `--smp` or `-c`
says how many, and `--cpuset` says which. It places them with hwloc,
the library that describes the machine's processors, spreading them
over the processor sockets and their caches.

### Fast and slow cores {index="cores!fast and slow"}

[Chapter](#ch:case-for-seastar)'s first exercise asked whether all of a
computer's cores are the same, and on many they aren't: Intel's recent
processors, in laptops and in some servers, mix fast *performance*
cores with slower *efficiency* cores, and so do most phones' processors.
On the laptop of this book's measurements, `lscpu -e` lists them:

```
CPU NODE SOCKET CORE L1d:L1i:L2:L3 ONLINE    MAXMHZ   MINMHZ
  0    0      0    0 0:0:0:0          yes 4700.0000 400.0000
  1    0      0    1 4:4:1:0          yes 4800.0000 400.0000
  2    0      0    2 8:8:2:0          yes 4700.0000 400.0000
  3    0      0    3 12:12:3:0        yes 4800.0000 400.0000
  4    0      0    4 64:64:8          yes 3700.0000 400.0000
  5    0      0    5 66:66:8          yes 3700.0000 400.0000
  6    0      0    6 68:68:8          yes 3700.0000 400.0000
  7    0      0    7 70:70:8          yes 3700.0000 400.0000
```

The first four run at up to 4.8 GHz, each with a level-2 cache of its
own; the last four at up to 3.7 GHz, and share one. Seastar doesn't know
the difference: hwloc can tell the kinds of core apart, but Seastar
doesn't ask, and gives every shard the same share of the work, as its
design assumes. Since a request that one shard sends to another waits
for that shard, the whole program is as fast as its slowest shards, as
[chapter](#ch:shared-nothing) warned:

Shoal 16, measured as in [chapter](#ch:designing-a-sharded-application),
with `valkey-benchmark`, in thousands of requests a second, the best of
three runs of 50 million requests, with the client on other cores, the
same ones in each half of the table:

| Shards on | `SET` | `GET` |
|---|---:|---:|
| One fast core (0) | 3,029 | 3,332 |
| One slow core (4) | 1,535 | 1,550 |
| Two fast cores (0, 1) | 4,165 | 4,443 |
| A fast and a slow core (0, 4) | 3,122 | 3,330 |
| Two slow cores (4, 5) | 2,699 | 2,813 |

A slow core does half of a fast one's work. Two shards on a fast and a
slow core do a quarter less than on two fast cores, and only 3% more
than one shard on a fast core alone: each shard gets an equal share of
the connections and the keys, so the fast shard waits for the slow one,
and the slow core adds almost nothing.

So on such a machine, a Seastar program should run on the fast cores
only, with `--cpuset 0-3` here, which is how this book's measurements
ran, and leave the slow ones to everything else, such as the clients of
a benchmark.

### Shared cores {index="cores!shared"}

Clouds have the same problem in other forms. A cloud machine's virtual
cores, *vCPUs*, are often the two hardware threads of a physical core,
which share its execution units, so two shards on the two threads of
one core each run slower than a shard that has the core to itself; and
a machine that isn't the whole physical server shares its cores, its
caches and its memory's bandwidth with other customers' machines, the
*noisy neighbors*, whose load comes and goes. A Seastar program can't
fix either, but should know them: a benchmark on a shared machine needs
more runs, and the reactor's statistics, below, count the time that the
hypervisor took away, [*steal time*]{index="steal time" .main}.

When the cores aren't the program's alone, as on a developer's laptop,
or in tests, `--overprovisioned`, which
[chapter](#ch:how-seastar-runs-your-code) explained, makes the shards
sleep rather than poll when they're idle, and doesn't pin them to their
cores; and `--poll-mode`, at the other extreme, makes them never sleep,
for the lowest latency on cores that nothing else uses.

### Containers {index="containers"}

In a container, Seastar finds its limits in the container's control
group, *cgroup*: it runs shards only on the cores of the cgroup's
`cpuset`, and takes the cgroup's memory limit as the machine's memory.
But a cgroup can also limit how much CPU time its processes get,
without saying on which cores: Docker's `--cpus 2`, or Kubernetes' CPU
limits, which set the cgroup's `cpu.max`. Seastar doesn't read that. It
sees all the machine's cores, starts a shard on each, and then the
kernel stops them all, for part of every period of 100 milliseconds,
when they used up the cgroup's share:

Shoal 16 with `--cpuset 0-3`, so four shards, in a cgroup with a quota
of two cores' worth of time (made with `systemd-run --user --scope -p
CPUQuota=200%`), against two shards on two cores without a quota, the
same amount of CPU; in thousands of requests a second, with the latency
of `GET`s without pipelining, in milliseconds:

| | `SET` | `GET` | Median | 99th percentile | Worst |
|---|---:|---:|---:|---:|---:|
| Four shards, a quota of two cores | 1,648 | 1,648 | 0.14 | 0.23 | 47 |
| Two shards, no quota | 4,440 | 4,757 | 0.047 | 0.063 | 0.34 |

With the same CPU, the throttled shards serve a third as many requests,
and a request that arrives when they're stopped waits for the next
period, up to 47 milliseconds, against a third of a millisecond. The
stall detector says nothing, since a shard that the kernel stopped isn't
running too long; it isn't running at all. The reactor counts the time
that it was ready to run but stopped as *steal time*, among its
statistics, below, which is how such throttling shows (exercise 2).

So in a container, give a Seastar program whole cores, with a `cpuset`
(Docker's `--cpuset-cpus`, or Kubernetes' static CPU manager), rather
than a share of the CPU time, or tell it, with `--smp`, how many shards
the share is worth.

## Memory {index="memory!in production"}

Seastar divides the machine's memory among the shards, keeping some for
the operating system: by default, the larger of 1.5 GB and 7% of it,
which `--reserve-memory` changes, while `-m` gives the program an exact
amount instead. Other processes on the machine, and the kernel's page
cache, which a Seastar program doesn't use for its own files
([chapter](#ch:files-and-storage)), must fit in what's left.

Each shard's memory comes from the processor socket that its core is
on, on a machine with several, a *NUMA* machine: Seastar asks the
kernel, with `mbind()`, to allocate it there, which `--mbind` controls,
and on by default. Two options matter on large production machines.
`--lock-memory` locks the program's memory, so that it's never swapped
out, which a server that owns the machine wants. And `--hugepages`,
given the path of a mounted `hugetlbfs`, takes the memory from the
kernel's pool of huge pages, of 2 MB, whose fewer entries in the
processor's address translation cache, the TLB, speed up a program that
touches much of its memory; without it, Seastar asks for transparent
huge pages, which the kernel provides when it can, unless the program
runs with `--overprovisioned`.

## The network's interrupts {index="interrupts"}

A network card interrupts a core when packets arrive, and the kernel's
work for them, the packets' path through TCP, runs on whichever core
handles the interrupt. On a busy server, that's a lot of work, which
takes time from the shard on that core, unevenly, since cards don't
spread their interrupts evenly. Seastar's [`scripts/perftune.py`]{index="`perftune.py`" .main} tunes
the machine for a Seastar program: it stops `irqbalance` from moving the
card's interrupts, binds them to cores that it chooses, and spreads the
kernel's further work on the packets, with Linux's *RPS*, among the
other cores. The cores for interrupts then run no shard: on a machine
with up to 4 cores, one hardware thread, on up to 16, one core, and on
larger machines, one core for every 16, divided among the NUMA nodes. It also tunes
disks, and some of `/proc/sys`, and `--dry-run` shows what it would do,
without doing it. `--get-cpu-mask` prints the cores that are then left
for the program. ScyllaDB's installation runs it, and writes that mask
to the file from which ScyllaDB's service takes its `--cpuset`.

## Choosing a reactor backend {index="reactor backend!choosing"}

[Chapter](#ch:how-seastar-runs-your-code) described the reactor's
backends, and measured them on Shoal: `epoll`, `linux-aio` and
`io_uring` were within 6% of one another. Seastar takes the first that
works, in this order: `io_uring`, if the kernel and Seastar's build
support it; `linux-aio`, if the system-wide limit on asynchronous I/O
requests, `/proc/sys/fs/aio-max-nr`, leaves room for the requests that
it reserves for every shard; and otherwise `epoll`. `linux-aio`
reserves 10,000 requests per shard for the network, by default
(`--max-networking-io-control-blocks`), so a machine with many cores
may need `aio-max-nr` raised, or Seastar falls back to `epoll`, as the
first line that it logs shows.

So the default is a good choice, and a program that wants another names
it. ScyllaDB does: its `main()` makes `linux-aio` the default, in the
words of the change that did it, in 2024, "the more mature AIO backend",
and uses `io_uring` only for its command-line tools. The asymmetric
backend, `asymmetric_io_uring`, is a different trade: it moves the
system calls and the kernel's work behind them to cores of their own,
which `--async-workers-cpuset` names, so it pays when the shards are
busy computing, and the machine has cores to spare, which would
otherwise run more shards. Without `--smp` or `--cpuset`, Seastar takes
those cores out of the shards' cores, and logs that it did; with
`--overprovisioned`, and neither option, it refuses to start; and it
warns if the workers' cores are also shards'.

## Building for production {index="optimization!production builds"}

[Chapter](#ch:futures-and-promises) showed that optimization matters
more for Seastar code than for most, because so much of it is small
templates that only inlining makes fast, and that inlining stops at the
edge of Seastar's compiled library. A production build can go further
than `RelWithDebInfo`'s `-O2`, in four ways, which ScyllaDB's release
build uses, all of them:

- **`-march`.** A compiler, by default, generates code that runs on
  every processor of the architecture, for x86 back to the first 64-bit
  ones, of 2003. `-march=x86-64-v3` lets it use the instructions of
  processors since about 2013, such as AVX2, at the price that the
  program won't run on older ones. ScyllaDB builds for `x86-64-v3` by
  default, and adds `-mpclmul`, an instruction for its CRC32 checksums,
  which that level doesn't include.
- **Inlining limits.** A compiler limits how much it inlines, to keep the
  code from growing too much, with limits tuned for ordinary code, where
  a function call costs little compared with the function's work. In
  Seastar code, with its stacks of tiny templates, the call is often the
  cost. ScyllaDB raised GCC's limit, `--param inline-unit-growth=300`,
  in 2018, when it found that GCC inlined too little in its large source
  files; and Clang's, `-mllvm -inline-threshold=2500`, in 2020, when
  ScyllaDB built with Clang ran 20% slower than with GCC, and with the
  new limit, 3%. The commit tells how the number was found, by binary
  search: at 5,000, one of ScyllaDB's files never finished compiling, and
  at 1,250, ScyllaDB lost 10% of its speed.
- **Link-time optimization (LTO).** As [chapter](#ch:futures-and-promises)
  explained, the compiler can't inline the code of Seastar's compiled
  library into the program, nor across the program's own source files;
  LTO optimizes them together, when the program is linked. With Clang's
  *ThinLTO*, `-flto=thin`, every object file holds the compiler's
  intermediate code, and the linker optimizes it in parallel, in pieces,
  which keeps the link of a large program fast enough. Seastar must be
  compiled with it too, since its library is half of what's optimized
  together; `-ffat-lto-objects` makes its object files hold machine code
  as well, so that a program linked without LTO can still use the
  library. ScyllaDB does exactly that.
- **Profile-guided optimization (PGO).** The compiler guesses which
  branches are taken and which code is hot; a profile replaces the
  guesses with facts. The program is built once with instrumentation,
  `-fprofile-instr-generate`, run on a typical workload, which writes the
  counts, and built again with them, `-fprofile-instr-use`. ScyllaDB
  keeps its profiles in its source repository, which its CI refreshes,
  and uses them conservatively, without two of Clang's uses of a profile
  (`-mllvm -pgso=false -mllvm -enable-value-profiling=false`): to compile
  cold code for size, a source of slowdowns, and to specialize code for
  the values that the training saw, since its training workload is,
  in the words of its build script, "not realistic and exhaustive enough".

We built Seastar and Shoal 16 with Clang each of these ways, and with
the profile from a run of the benchmark below, and measured them as
[chapter](#ch:designing-a-sharded-application) did, on one shard, in
thousands of requests a second, the best of three runs:

| Build (Clang) | `SET` | `GET` |
|---|---:|---:|
| `-O2`, as `RelWithDebInfo` | 2,984 | 3,251 |
| With `-march=x86-64-v3` | 3,006 | 3,277 |
| With `-mllvm -inline-threshold=2500` | 3,119 | 3,360 |
| With ThinLTO | 3,024 | 3,277 |
| With PGO and ThinLTO | 3,119 | 3,389 |

Each helps, a little: the inlining limit and the profile 3% to 5%, and
`-march` and LTO alone about 1%. These are the gains of one small
server; ScyllaDB's 20%, from the inlining limit alone, came from a much
larger program, and a profile like
[chapter](#ch:debugging-and-profiling)'s, which found Shoal's busiest
function waiting for memory, shows what no compiler option can speed
up. Their cost is in the build: LTO makes the link slower, and PGO
needs a training run, a profile to keep in step with the code, and the
care that the training resembles the real work, since code that the
training never ran is compiled as cold. So they're for the build that
goes to production, not for the build of every day. Measure them on
your own program, as here, with runs long enough, and repeated: the
differences are small enough for noise to hide them.

## What the reactor knows {index="reactor!statistics"}

Each shard's reactor counts what it does, and `engine()`, which
[chapter](#ch:stalls) introduced, gives the counts to the shard's code:
`uptime()`, since the reactor started; `total_busy_time()`, the time it
spent running tasks or polling with something found, and
`total_idle_time()`, the rest; `total_cpu_time()`, the CPU time that its
thread used, and `total_steal_time()`, the time that it was ready to
run but didn't, which on a cloud machine is the hypervisor's; `polls()`,
how many times it polled; `get_sched_stats()`, its tasks;
`get_io_stats()`, its I/O; and `cxx_exceptions()`, how many C++
exceptions were thrown on the shard, which, since exceptions are slow,
should be few. The same numbers are among the metrics of
[chapter](#ch:observability), which is where a deployed server's
operators see them; the functions are for the program itself, to log
them, or act on them. ScyllaDB reads `get_io_stats()` and
`get_sched_stats()`, and Redpanda `total_busy_time()`.

A shard that's idle can also do work of its own.
[`set_idle_cpu_handler()`]{index="`seastar::set_idle_cpu_handler()`" .main},
from `<seastar/core/idle_cpu_handler.hh>`, installs a function, which
the reactor calls when it has nothing else to do, before it sleeps. The
function does a little of its low-priority work, calls the function
that it's given, to ask whether real work arrived, and returns
`idle_cpu_handler_result::interrupted_by_higher_priority_task` when it
did, or `no_more_work` when it's done, and the reactor may sleep.
ScyllaDB uses it for one thing, if configured to: to defragment the
memory in which it keeps its cache and its data, work that otherwise
competes with requests.

## Exercises

1. Write a program with an idle CPU handler that counts primes, a
   little at a time, while a timer that should fire every millisecond
   notes how late it comes. Compare a handler that asks the reactor for
   work often, every hundred numbers, with one that asks every million.

2. Write a program that works half of the time, for a millisecond, and
   sleeps for a millisecond, for two seconds, and then prints what its
   shard's reactor says about itself: its busy, idle, awake, CPU and
   steal times, polls and tasks. Run it again under a quota of half a
   core, with `systemd-run --user --scope -p CPUQuota=50%`. What
   changes?

3. Under the quota of two cores above, start Shoal with `--smp 2`, and
   measure it again.
