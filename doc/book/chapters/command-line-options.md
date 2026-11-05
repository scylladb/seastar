# Command-line options {#app:command-line-options}

A program that `app_template` starts ([chapter](#ch:getting-started))
accepts Seastar's options besides its own. This appendix lists them,
as of the Seastar that this book was tested with, grouped by subject,
each with its default, and with the chapter that explains it, where
one does. `--help-seastar` prints the list for the Seastar that a
program was built with, which may have more or fewer: some options
exist only in builds that enabled a feature, such as DPDK or heap
profiling.

An option's value follows it, after a space or an `=`: `--smp 4` and
`--smp=4` are the same. Some options are flags, given without a value,
such as `--poll-mode`; others take a yes-or-no value, written `1` or
`0`, such as `--thread-affinity 0`. A size of memory, as for `-m`, may
end with `k`, `M`, `G` or `T`, which are powers of 1024: `-m4G` is four
gibibytes.

## Help

`-h`, `--help`
:   []{index="command-line options!`--help`"}Print the program's own options, and exit
    ([chapter](#ch:getting-started)).

`--help-seastar`
:   []{index="command-line options!`--help-seastar`"}Print Seastar's options, and exit.

`--help-loggers`
:   []{index="command-line options!`--help-loggers`"}Print the names of the program's loggers, Seastar's and its own,
    and exit ([chapter](#ch:getting-started)).

## Shards, cores and memory

`-c`, `--smp` *N* (default: one per core)
:   []{index="command-line options!`-c`, `--smp`"}The number of shards ([chapter](#ch:tutorial-introduction),
    [chapter](#ch:deploying-and-tuning)).

`--cpuset` *list* (default: all)
:   []{index="command-line options!`--cpuset`"}The cores to run the shards on, in the format of `cpuset(7)`, such
    as `0,2-5` ([chapter](#ch:shared-nothing)).

`--thread-affinity` *0|1* (default 1)
:   []{index="command-line options!`--thread-affinity`"}Pin each shard's thread to its core ([chapter](#ch:testing)).

`--overprovisioned`
:   []{index="command-line options!`--overprovisioned`"}For machines whose cores are shared with other programs, such as a
    laptop or a test machine: the same as `--idle-poll-time-us 0
    --thread-affinity 0 --poll-aio 0`, for each of those not given
    explicitly; it also lets `-c` ask for more shards than there are
    cores, and doesn't ask for transparent huge pages
    ([chapter](#ch:how-seastar-runs-your-code)).

`-m`, `--memory` *size* (default: all, less a reserve)
:   []{index="command-line options!`-m`, `--memory`"}The memory for all the shards together, divided equally among them
    ([chapter](#ch:memory)).

`--reserve-memory` *size*
:   []{index="command-line options!`--reserve-memory`"}Without `-m`, how much memory to leave to the operating system and
    other programs ([chapter](#ch:memory)).

`--lock-memory` *0|1* (default 0)
:   []{index="command-line options!`--lock-memory`"}Lock the program's memory, so that it's never swapped out
    ([chapter](#ch:deploying-and-tuning)).

`--hugepages` *path*
:   []{index="command-line options!`--hugepages`"}Take the memory from huge pages, through a `hugetlbfs` file system
    mounted at the path ([chapter](#ch:deploying-and-tuning)).

`--mbind` *0|1* (default 1)
:   []{index="command-line options!`--mbind`"}Ask the kernel to allocate each shard's memory on the shard's own
    NUMA node ([chapter](#ch:deploying-and-tuning)).

`--allow-cpus-in-remote-numa-nodes` *0|1* (default 1)
:   []{index="command-line options!`--allow-cpus-in-remote-numa-nodes`"}If a core has no NUMA node with memory of its own, give its shard
    memory from another node, rather than fail.

`--num-io-groups` *N* (default: one per NUMA node)
:   []{index="command-line options!`--num-io-groups`"}The number of groups among which the shards divide each disk's
    capacity ([chapter](#ch:io-scheduler)).

## The reactor

`--reactor-backend` *name* (default: the first of `io_uring`, `linux-aio` and `epoll` that works)
:   []{index="command-line options!`--reactor-backend`"}How the reactor waits for I/O: `io_uring`, `linux-aio`, `epoll`, or
    `asymmetric_io_uring` ([chapter](#ch:how-seastar-runs-your-code),
    [chapter](#ch:deploying-and-tuning)).

`--async-workers-cpuset` *list*
:   []{index="command-line options!`--async-workers-cpuset`"}The cores for the kernel's I/O workers of the `asymmetric_io_uring`
    backend, which requires it
    ([chapter](#ch:how-seastar-runs-your-code)).

`--task-quota-ms` *ms* (default 0.5)
:   []{index="command-line options!`--task-quota-ms`"}How long the reactor runs tasks before it polls again
    ([chapter](#ch:how-seastar-runs-your-code)).

`--idle-poll-time-us` *µs* (default 200)
:   []{index="command-line options!`--idle-poll-time-us`"}How long an idle reactor keeps polling before it sleeps
    ([chapter](#ch:how-seastar-runs-your-code)).

`--poll-mode`
:   []{index="command-line options!`--poll-mode`"}Poll forever, never sleeping: a core at 100% all the time
    ([chapter](#ch:how-seastar-runs-your-code)).

`--poll-aio` *0|1* (default 1)
:   []{index="command-line options!`--poll-aio`"}Busy-poll for completed disk I/O ([chapter](#ch:testing)).

`--max-task-backlog` *N* (default 1000)
:   []{index="command-line options!`--max-task-backlog`"}When it's time to poll, but a task queue holds more tasks than this,
    keep running tasks rather than poll, and log a warning, "Too long
    queue accumulated".

`--abort-on-too-long-task-queue` *0|1* (default 0)
:   []{index="command-line options!`--abort-on-too-long-task-queue`"}Abort, instead of only logging, when a queue exceeds
    `--max-task-backlog`.

`--no-handle-interrupt`
:   []{index="command-line options!`--no-handle-interrupt`"}Ignore `SIGINT`, rather than stop the program; meant for running it
    under gdb.

`--enable-glibc-exception-scaling-workaround` *0|1* (default 1)
:   []{index="command-line options!`--enable-glibc-exception-scaling-workaround`"}Throwing an exception searches the program's loaded libraries, and
    glibc's function for that takes a lock that all threads share, so
    that shards throwing at once wait for each other. Seastar replaces
    the function with one that takes no lock.

## Stalls and memory diagnostics

`--blocked-reactor-notify-ms` *ms* (default 25)
:   []{index="command-line options!`--blocked-reactor-notify-ms`"}The stall detector's threshold ([chapter](#ch:stalls)).

`--blocked-reactor-reports-per-minute` *N* (default 5)
:   []{index="command-line options!`--blocked-reactor-reports-per-minute`"}The most stall reports to log per minute
    ([chapter](#ch:how-seastar-runs-your-code)).

`--blocked-reactor-report-format-oneline` *0|1* (default 1)
:   []{index="command-line options!`--blocked-reactor-report-format-oneline`"}Print a stall report's backtrace on one line.

`--abort-on-seastar-bad-alloc`
:   []{index="command-line options!`--abort-on-seastar-bad-alloc`"}Abort when an allocation fails, rather than throw `std::bad_alloc`
    ([chapter](#ch:memory)).

`--dump-memory-diagnostics-on-alloc-failure-kind` *kind* (default `critical`)
:   []{index="command-line options!`--dump-memory-diagnostics-on-alloc-failure-kind`"}When an allocation fails, log the state of the shard's memory:
    `none`, `critical` (only for allocations in a critical section), or
    `all` ([chapter](#ch:memory)).

`--heapprof` *bytes* (default 0, off)
:   []{index="command-line options!`--heapprof`"}Sample one allocation in about this many bytes, for heap profiling;
    only in a Seastar built with it ([chapter](#ch:memory),
    [chapter](#ch:memory-leaks)).

## Disks

`--io-properties-file` *path*, `--io-properties` *YAML*
:   []{index="command-line options!`--io-properties-file`"}[]{index="command-line options!`--io-properties`"}The disks' measured capacity, from `iotune`, in a file or as a
    string ([chapter](#ch:io-scheduler)).

`--io-latency-goal-ms` *ms* (default: 1.5 times `--task-quota-ms`)
:   []{index="command-line options!`--io-latency-goal-ms`"}[]{index="command-line options!`--task-quota-ms`"}The latency that the I/O scheduler aims for
    ([chapter](#ch:io-scheduler)).

`--io-flow-rate-threshold` *ratio* (default 1.1)
:   []{index="command-line options!`--io-flow-rate-threshold`"}How far the rate of dispatching requests may exceed the rate of
    their completion before the scheduler slows down
    ([chapter](#ch:io-scheduler)).

`--io-completion-notify-ms` *ms* (default: off)
:   []{index="command-line options!`--io-completion-notify-ms`"}Log a warning for a disk request that took longer than this; the
    threshold doubles after each warning, so that a slow disk doesn't
    flood the log.

`--kernel-page-cache` *0|1* (default 0)
:   []{index="command-line options!`--kernel-page-cache`"}Read and write files through the kernel's page cache, not with
    direct I/O ([chapter](#ch:files-and-storage)).

`--relaxed-dma`
:   []{index="command-line options!`--relaxed-dma`"}On a file system that doesn't support direct I/O, such as `tmpfs`,
    use buffered I/O rather than fail.

`--unsafe-bypass-fsync` *0|1* (default 0)
:   []{index="command-line options!`--unsafe-bypass-fsync`"}Make `flush()` of a file return at once, without making its data
    durable: fast, and unsafe, for tests
    ([chapter](#ch:files-and-storage)).

`--aio-fsync` *0|1* (default 1 if the kernel supports it)
:   []{index="command-line options!`--aio-fsync`"}Send `fsync()` through the kernel's asynchronous I/O, rather than
    through a system-call thread.

`--linux-aio-nowait` *0|1* (default: detected)
:   []{index="command-line options!`--linux-aio-nowait`"}Ask the kernel not to block when it submits asynchronous I/O.

`--force-aio-syscalls` *0|1* (default 0)
:   []{index="command-line options!`--force-aio-syscalls`"}Collect completed I/O with a system call every time, which is
    slower, but shows it in `strace`.

`--max-networking-io-control-blocks` *N* (default 10000), `--reserve-io-control-blocks` *N* (default 0)
:   []{index="command-line options!`--max-networking-io-control-blocks`"}[]{index="command-line options!`--reserve-io-control-blocks`"}For the `linux-aio` backend: the kernel's I/O control blocks to
    allocate per shard, which limit the sockets per shard, and those to
    leave to other programs ([chapter](#ch:deploying-and-tuning)).

## Networking

`--network-stack` *name* (default `posix`)
:   []{index="command-line options!`--network-stack`"}`posix`, the kernel's TCP/IP, or `native`, Seastar's own
    ([chapter](#ch:networking)).

`--crypto-provider` *name* (default `gnutls`)
:   []{index="command-line options!`--crypto-provider`"}The library for TLS: `gnutls` or `openssl`
    ([chapter](#ch:networking)).

The native stack has options of its own ([chapter](#ch:networking)):
the device, with `--dpdk-pmd` for a network card driven by DPDK (only
in a Seastar built with DPDK, with `--dpdk-port-index` to choose the
card's port and `--hw-fc` to turn its flow control on or off), or `--tap-device` (default
`tap0`) for a Linux tap device; the address, with `--dhcp` (default 1)
or `--host-ipv4-addr`, `--gw-ipv4-addr` and `--netmask-ipv4-addr`; and
tuning, with `--lro`, `--hw-queue-weight` and `--udpv4-queue-size`. A
tap device is driven through virtio, whose own options, `--event-index`,
`--csum-offload`, `--tso`, `--ufo` and `--virtio-ring-size`, turn its
features on or off.

## Logging

`--default-log-level` *level* (default `info`)
:   []{index="command-line options!`--default-log-level`"}The level of every logger whose level isn't set otherwise: `trace`,
    `debug`, `info`, `warn` or `error` ([chapter](#ch:getting-started)).

`--logger-log-level` *levels*
:   []{index="command-line options!`--logger-log-level`"}The levels of individual loggers, as `name=level`, several separated
    by colons; may be repeated ([chapter](#ch:getting-started)).

`--log-to-stdout` *0|1* (default 1), `--logger-ostream-type` *where* (default `stderr`)
:   []{index="command-line options!`--log-to-stdout`"}[]{index="command-line options!`--logger-ostream-type`"}Whether to write the log to a stream, and which: `stdout`,
    `stderr`, or `none` ([chapter](#ch:observability)).

`--logger-stdout-timestamps` *style* (default `real`)
:   []{index="command-line options!`--logger-stdout-timestamps`"}The time on each line of the log: `real`, the date and time;
    `boot`, the time since the machine started; or `none`
    ([chapter](#ch:observability)).

`--log-to-syslog` *0|1* (default 0)
:   []{index="command-line options!`--log-to-syslog`"}Also send the log to syslog ([chapter](#ch:observability)).

`--log-with-color` *0|1* (default 0)
:   []{index="command-line options!`--log-with-color`"}Color each line's level.

## Metrics

Seastar has no options for its Prometheus exporter, whose address and
port the program chooses ([chapter](#ch:observability)). Its metrics
options are left from its collectd exporter, which was removed:

`--metrics-hostname` *name* (default: the machine's name)
:   []{index="command-line options!`--metrics-hostname`"}A host name that Seastar stores, and no longer uses itself.

`--collectd`, `--collectd-address`, `--collectd-hostname`, `--collectd-poll-period`
:   []{index="command-line options!`--collectd`"}[]{index="command-line options!`--collectd-address`"}[]{index="command-line options!`--collectd-hostname`"}[]{index="command-line options!`--collectd-poll-period`"}Accepted, and ignored; `--collectd 1` logs a warning.
