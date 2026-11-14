# Getting started {#sol:getting-started}

## Exercise 1

On the laptop of [chapter](#ch:case-for-seastar), without a compiler
cache, building the library alone, about a hundred source files, took 88
seconds in `release` mode and 65 in `dev` mode. (The default build also
compiles Seastar's tests, demos and applications, about 150 files more,
which a program that only uses Seastar doesn't need.) `dev` compiles
faster because it optimizes less, as the chapter's table of modes says.

Built against each, `pong` came out very different in size: 103 MB
against `release`, and 181 KB against `dev`. The `release` program
contains the whole Seastar library, with its debugging information,
because `release` builds Seastar as a static library,
`build/release/libseastar.a`, of 252 MB, whose code the linker copies
into the program. `dev` builds it as a shared library instead,
`build/dev/libseastar.so`, of 10 MB, which the program only refers to.
(`debug` does the same as `dev`.) Building `pong` against `pkg-config
--cflags --libs --static build/dev/seastar.pc`, with the path to the
`.pc` file instead of a package name, works as the exercise says.

The `dev` program then doesn't start:

```
./pong: error while loading shared libraries: libseastar.so: cannot open shared object file: No such file or directory
```

The program needs the shared library at run time, and the dynamic linker
looks for it only in the system's directories, where nothing installed
it. Telling it where to look, with
`LD_LIBRARY_PATH=$HOME/seastar/build/dev ./pong`, makes it run. (An
installed `dev` Seastar would put the library where the system finds it,
or a program could record its path, its "rpath", when it's linked.)

So `dev` is the mode for the edit, compile and test cycle: Seastar builds
faster, programs link much faster against a shared library, and they're
small. `release` is for programs that you measure, or deploy: one file,
which needs nothing else at run time.

## Exercise 2

Here is Shoal version 0 with the new option. Its `options` get a string,
with its option and default value, and every shard logs it:

```{.cpp include="examples/solutions/getting-started-ex2.cc"}
```

The lambda given to `smp::invoke_on_all()` captures `opts` by reference,
which is safe because `shoal_main()` waits for all the shards to finish
before it returns, so `opts` outlives every use. Without `--greeting`,
every shard says `hello`; with `--greeting hi`, `hi`; with
`SEASTAR_OPTIONS='--greeting hi'`, `hi` too. With both, as in
`SEASTAR_OPTIONS='--greeting hi' ./shoal --greeting hey`, the command line
wins: Shoal says `hey`. `SEASTAR_OPTIONS` is read as if its options came
before the command line's, so for an option given twice, the later one,
the command line's, counts.

A greeting with a space can be given in `SEASTAR_OPTIONS`, quoted, as on a
shell's command line: `SEASTAR_OPTIONS='--greeting "good morning"'` works,
because Seastar splits the variable the way a shell splits words. Without
the inner quotes, `SEASTAR_OPTIONS='--greeting good morning'`, Shoal
refuses to start:

```
error in SEASTAR_OPTIONS: too many positional options have been specified on the command line
```

since "morning" is then a word of its own, which no option takes.
`SEASTAR_OPTIONS` isn't only for Seastar's own options, despite its name:
it can give any of the program's options, which is handy for setting
options in a test script or a service's environment, without changing
its command line.

## Exercise 3

With `--logger-log-level shoal=trace`, Shoal logs exactly what it logs
with `shoal=debug`: Shoal has no messages at the `trace` level, so asking
for them adds nothing. The level only sets the lowest level that a logger
prints; the messages themselves are whatever the program logs.

With `--default-log-level debug`, every logger prints its debug messages,
and the extra ones come from Seastar's own loggers: about fifteen from
the `seastar` logger, and three from `io`, as Seastar sets up its shards
and its I/O queues. Among them are the lines that say where each shard
runs:

```
DEBUG ... seastar - Assign CPU0 to NUMA0
DEBUG ... seastar - CPU0 allocated 536870912 bytes on NUMA0
```

With `-c2`, Seastar chose CPUs 0 and 4, and gave each shard 536,870,912
bytes, 512 MB; with `-c4`, CPUs 0, 2, 4 and 6, with 256 MB each. So `-m`,
here 1 GB, is the program's total memory, which Seastar divides equally
among the shards, as [chapter](#ch:shared-nothing) shows in more detail.

`lscpu -e` showed in [chapter](#ch:case-for-seastar)'s first exercise that
this laptop's CPUs 0 to 3 are fast and 4 to 7 slow. So Seastar didn't
choose the fast cores: it spread the shards evenly over the machine, as
if every core were alike, two fast and two slow with `-c4`. Seastar
doesn't know how fast each core is; on such a machine, choose the cores
yourself, with `--cpuset 0-3`, as this book's measurements do.

## Exercise 4

`--help-seastar` lists `--reactor-backend`, which on this machine offers
`io_uring`, the default, `asymmetric_io_uring`, `linux-aio` and `epoll`.
Each of them starts Shoal, which logs it first:

```
INFO  ... seastar - Reactor backend: epoll
```

except `asymmetric_io_uring`, which refuses:

```
No CPUs specified for asymmetric_io_uring workers. Please see --async-workers-cpuset option.
```

This backend doesn't do the kernel's I/O work on the shard's own CPU:
it hands it to kernel threads that run on other CPUs, which must be
chosen with `--async-workers-cpuset`. With `--async-workers-cpuset 4`, it
starts. Which backends a machine offers depends on its kernel, and on how
Seastar was built: `io_uring` needs a kernel and a `liburing` that
support it. [Chapter](#ch:how-seastar-runs-your-code) explains what the
backends do, and when each is the better choice.
