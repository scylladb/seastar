# Observability {#ch:observability}

A server in production is a black box that someone must look into: to
see that it's healthy, to understand why it got slow last Tuesday, and to
plan how many machines it will need next year. It answers in three ways:
its log, which tells of the events worth telling; its *metrics*, numbers
that it keeps about itself, such as requests served, bytes written or
memory used, which a monitoring system collects every few seconds and
draws as graphs; and tracing, the record of what happened to a single
request, or to a single disk access. Earlier chapters used Shoal's log
since [chapter](#ch:getting-started), and mentioned some of the metrics
that Seastar keeps about itself: the I/O scheduler's, in
[chapter](#ch:io-scheduler), the scheduling groups', in
[chapter](#ch:scheduling-groups), the memory's, in
[chapter](#ch:memory), and the stall detector's, in
[chapter](#ch:stalls). This chapter shows how to collect them, and how a
program adds metrics of its own; Shoal's version 15 does, and serves
them all on its admin API. It also answers a question that every new
Seastar user asks, sooner or later: why does `top` say that a server
that's hardly busy uses all of its CPUs?

## Metrics {index="metrics"}

A metric is a named number, with *labels*, pairs of names and values,
that tell its instances apart: Seastar labels each metric with its
shard, so a program with eight shards has eight of each. The number is
one of a few kinds. A *counter* only grows, such as the number of
requests served, and what matters is its rate, how much it grew in the
last minute, which the monitoring system computes. A *gauge* goes up and
down, such as the number of open connections. A *histogram* counts
events in buckets of their size, such as requests by how long they took,
from which the monitoring system estimates percentiles.

A program registers its metrics in a
[`metrics::metric_groups`]{index="`seastar::metrics::metric_groups`" .main},
from `<seastar/core/metrics_registration.hh>`, an object whose
destruction unregisters them, so it's usually a member of the object
that the metrics describe. Shoal's are in each shard's store, which
registers them when Shoal starts, with the functions of
`<seastar/core/metrics.hh>`:

```{.cpp include="shoal/v15/commands.cc" snippet="register-metrics"}
```

Each metric is made by a function of its kind:
[`make_gauge()`]{index="`seastar::metrics::make_gauge()`" .main},
[`make_counter()`]{index="`seastar::metrics::make_counter()`" .main},
[`make_histogram()`]{index="`seastar::metrics::make_histogram()`" .main},
and a few that add a unit, such as
[`make_current_bytes()`]{index="`seastar::metrics::make_current_bytes()`"}
and [`make_total_bytes()`]{index="`seastar::metrics::make_total_bytes()`"},
a gauge and a counter of bytes. A metric reads a variable, which it is
given by reference, such as the store's `evicted_keys`, or calls a
function, such as the lambda that returns the size of the shard's
table. Either way, nothing happens until the metrics are collected:
counting is the program's own `++`, as cheap as it can be, and the
metric only reads the count, on its shard, when someone asks.

The calls of each command are a counter with a label, `command`, made
with a
[`label`]{index="`seastar::metrics::label`"}, whose call operator gives
a label its value. Every call of a command adds one to its shard's
entry of the store's `calls`:

```{.cpp include="shoal/v15/main.cc" snippet="count-calls"}
```

`set_skip_when_empty()` leaves out a counter that is still 0, so a
command that nobody called isn't reported at all, and `aggregate()`
sums the counters of all the shards into one, when they are collected,
since which shard received a call says little. A histogram's function
returns a
[`metrics::histogram`]{index="`seastar::metrics::histogram`"}, from
`<seastar/core/metrics_types.hh>`, which is only the numbers: a count
and a sum of the events, and the buckets. Keeping the buckets is the
program's job, and Shoal's histogram of the log's flushes counts
latencies in powers of two of microseconds, with one increment each,
and builds the buckets only when it's collected:

```{.cpp include="shoal/v15/latency.hh" snippet="latency-histogram"}
```

The statistics of the log are in the store, not in the log, because a
rewrite replaces the log with a new one, and a counter must never go
back.

### Metrics from one snapshot {index="metrics!collector"}

The replication metrics of a primary describe one shard's feed: how
many bytes it received, and how far behind the replica is. Each is read
separately, so the feed may change between the reads, and the lag might
not match the bytes. A
[`collector`]{index="`seastar::metrics::collector`"}, new in Seastar
in 2026, takes one snapshot of something, when the shard's metrics are
collected, before any of them is read, and the metrics made from it read
the snapshot:

```{.cpp include="shoal/v15/replication.cc" snippet="replication-metrics"}
```

## Prometheus {index="Prometheus"}

Seastar serves the metrics in the format of
[Prometheus]{index="Prometheus"}, the monitoring system that ScyllaDB and
Redpanda both use, over HTTP. It doesn't start a server for it by itself:
the program gives it one. ScyllaDB starts an HTTP server just for its
metrics, on port 9180 by default, with
`prometheus::start()`; Shoal adds the route `/metrics` to its admin
API ([chapter](#ch:http)), with
[`prometheus::add_prometheus_routes()`]{index="`seastar::prometheus::add_prometheus_routes()`" .main},
from `<seastar/core/prometheus.hh>`:

```{.cpp include="shoal/v15/main.cc" snippet="prometheus"}
```

The prefix goes before every metric's name, Seastar's own too, so that
all the metrics of a program are told apart from those of the others
that the same Prometheus collects: ScyllaDB's begin with `scylla_`, and
Shoal's with `shoal_`. A query returns the metrics of all the shards,
which the shard that serves it collects from the others:

```
$ shoal -c2 --admin-port 8080 &
$ redis-cli MSET a 1 b 2 c 3 d 4
OK
$ redis-cli GET a
"1"
$ redis-cli GET b
"2"
$ curl 'localhost:8080/metrics?__name__=commands_calls'
# HELP shoal_commands_calls Calls of the command
# TYPE shoal_commands_calls counter
shoal_commands_calls{command="mset"} 1
shoal_commands_calls{command="get"} 2
$ curl 'localhost:8080/metrics?__name__=store_keys&__help__=false'
# TYPE shoal_store_keys gauge
shoal_store_keys{shard="0"} 0.000000
shoal_store_keys{shard="1"} 4.000000
```

The query can ask for less, as Seastar's `doc/prometheus.md` describes:
`__name__` selects metrics by name, or by the beginning of their name,
with a `*`, and may be given several times; a label's name selects by
that label's value, a regular expression; `__help__=false` leaves out
the descriptions; and `__aggregate__=false` shows each shard's own
values of the metrics that are summed. Prometheus itself asks for all
of them, every few seconds, as its configuration says, which can also
give it these parameters:

```yaml
scrape_configs:
  - job_name: shoal
    static_configs:
      - targets: ['localhost:8080']
    params:
      __name__: ['commands*', 'reactor_utilization']
```

A program with many shards has many metrics: Seastar alone registers
109 for each shard of Shoal's, besides Shoal's own. Asking only for
those that a dashboard draws makes each collection cheaper, for the
program and for Prometheus.

The program can also change its metrics while it runs, as Prometheus's
own [*relabeling*]{index="relabeling" .main} does:
[`metrics::set_relabel_configs()`]{index="`seastar::metrics::set_relabel_configs()`" .main},
from `<seastar/core/metrics_api.hh>`, takes a list of rules, each of
which matches metrics by their names and labels, with a regular
expression, and drops them, keeps them, changes a label's value, or
removes a label. ScyllaDB reads such rules from a file that its
configuration names, so that its operators can drop the metrics that
they don't collect.

## How much CPU is Seastar using? {index="CPU usage"}

As [chapter](#ch:how-seastar-runs-your-code) explained, a shard that runs
out of work doesn't sleep at once: it polls for new work for a while
first, 200 microseconds by default (2 milliseconds in a virtual
machine), because waking a sleeping thread
takes longer than that. A shard that gets a little work every 100
microseconds therefore never sleeps, and to the operating system, and to
`top`, it uses its whole core, however little work it does. Seastar's
own metrics tell the two apart:

- [`reactor_utilization`]{index="metrics!`reactor_utilization`"}, a
  gauge: the share of the last five seconds in which the shard had
  work. A shard counts as idle from the moment that a poll finds no
  work, so polling while idle doesn't count.
- `reactor_cpu_busy_ms`, a counter of the same: the milliseconds in
  which the shard had work.
- `reactor_cpu_used_time_ms`, a counter of the CPU time of the shard's
  thread, as the operating system counts it, polling and all: what
  `top` shows.
- `reactor_sleep_time_ms_total` and `reactor_awake_time_ms_total`, the
  wall-clock time asleep and awake, and `reactor_cpu_steal_time_ms`,
  the time that the shard wanted to run while something else ran on its
  core, which on a virtual machine includes the hypervisor's other
  guests.

We ran Shoal with two shards, on two cores of their own, and a client on
another core, which sent `GET` requests for 1,000 keys, so that both
shards had work: the shard that holds the client's connection reads
every request, executes those of its own keys, and sends the others to
the other shard. Over ten seconds, `top`, as computed from the thread's
CPU time, and Shoal's metrics said, in the best of three runs, for
shard A, which has the client's connection, and shard B, the other:

| Clients | Idle polling | Requests/s | A: `top` | A: utilization | B: `top` | B: utilization |
|---:|---|---:|---:|---:|---:|---:|
| 0 | default | 0 | 0% | 0% | 0% | 0% |
| 1 | default | 98,580 | 87% | 48% | 100% | 5% |
| 1 | none | 62,624 | 45% | 31% | 13% | 6% |
| 8 | default | 258,148 | 85% | 65% | 85% | 65% |
| 8 | none | 252,040 | 70% | 62% | 70% | 62% |

An idle Shoal uses no CPU at all. With one client, shard B received a message every 20 microseconds or so, did 5% of real work,
and polled the rest of the time: `top` said 100%. With
`--idle-poll-time-us 0`, the shards sleep as soon as they run
out of work, `top` agrees with the metrics, give or take the cost of
going to sleep and waking up, and that cost shows elsewhere: the one
client got 36% fewer replies a second, since each of its requests
now waited for a shard to wake up. With eight clients, the shards had
work two thirds of the time, and polled through much of the rest: `top`
said 85%, and without idle polling 70%, for 2% fewer requests.

So the CPU that a Seastar server needs is its utilization; what `top`
shows is the CPU that it takes when it can, which on a dedicated
machine is free. On a machine shared with other programs, idle polling
takes from them, which is what `--overprovisioned`, which sets the idle
polling to zero among other things, and `--idle-poll-time-us` are for.
`perf stat`, as [chapter](#ch:futures-and-promises) used it, counts what
the CPU really did, in instructions and cycles, which is the measure to
compare two versions of a server by, for a given load.

## Logging, again {index="logging"}

Shoal has logged since [chapter](#ch:getting-started), with a
`seastar::logger`, whose level `--logger-log-level` and
`--default-log-level` set. A few more things matter in production.

**Where the log goes.** By default to the standard error, with each
line's time and shard; `--logger-ostream-type stdout` sends it to the
standard output instead, `--log-to-syslog` to the system's log, and
`--logger-stdout-timestamps` chooses between real time, the time since
the machine booted, and none, for a log that a service manager such as
systemd stamps itself.

**A level that changes while the program runs.** A logger is one
object, which all the shards share, and its level is an atomic
variable, so changing it on any shard changes it on all of them, and
[`global_logger_registry()`]{index="`seastar::global_logger_registry()`"},
from `<seastar/util/log.hh>`, finds a logger by its name.
ScyllaDB's REST API has an operation that sets a logger's level, which
operators use to turn on `debug` for a while without restarting a node;
exercise 1 adds one to Shoal's admin API.

**A message that may come in floods.** A server that logs every failed
connection logs thousands of lines a second when the network fails, and
the log then hides more than it shows, and costs the CPU that the
server needs. A
[`logger::rate_limit`]{index="`seastar::logger::rate_limit`" .main}
lets one message through each interval, and counts the others, which it
reports with the next message that it lets through:

```{.cpp include="shoal/v15/main.cc" snippet="rate-limit"}
```

The rate limit is `static thread_local`: one for each shard, kept
between calls, since a shard's messages are its own business, and a
`rate_limit` isn't safe to share between threads.

Logging costs nothing when the level is off: the logger checks the level
before it formats anything, so a `debug()` call with expensive
arguments costs a comparison when `debug` is off, but its arguments are
still evaluated, so a call that computes them is better guarded with
`is_enabled()`.

## Tracing {index="tracing"}

Metrics count; a log tells of events; neither shows what happened to one
request. Seastar has no tracing for an application's requests: ScyllaDB
traces a CQL request, when asked to, with tracing of its own, which
writes each step to tables that the user can read.
What Seastar has is tracepoints of LTTng, Linux's tracer for programs,
in its I/O scheduler, which Seastar is built with by default: for each
disk request, `io_queue_queued` records its file descriptor, an
identifier, its direction (0 for a write), its class, its offset and its
length, and `io_queue_dispatched`, `io_queue_completed` and
`io_queue_cancelled` its identifier. A tracepoint costs a few
nanoseconds when nobody traces, Seastar's documentation says. LTTng's
session daemon, `lttng-sessiond`, must run before the program starts,
and then a *session* records the tracepoints of any program, which
`babeltrace2` prints:

```
$ lttng-sessiond --daemonize
$ lttng create shoal-io --output=/tmp/trace
$ lttng enable-event -u 'seastar_io:*'
$ lttng add-context -u -t procname
$ lttng start
$ shoal -c2 --appendfsync always &
$ redis-cli SET a 1
$ redis-cli SET b 2
$ lttng stop
$ babeltrace2 /tmp/trace
[12:57:34.871359827] (+?.?????????) host seastar_io:io_queue_queued:
  { cpu_id = 0 }, { procname = "shoal" }, { fd = 43,
  req_id = 0x60000018BB80, direction = 0, pclass = 0, offset = 0,
  length = 4096 }
[12:57:34.871370556] (+0.000010729) host seastar_io:io_queue_dispatched: ...
[12:57:34.871491264] (+0.000120708) host seastar_io:io_queue_completed: ...
[12:57:35.440848310] (+0.569357046) host seastar_io:io_queue_queued:
  { cpu_id = 1 }, { procname = "reactor-1" }, { fd = 44,
  req_id = 0x601000155B80, direction = 0, pclass = 0, offset = 0,
  length = 4096 }
[12:57:35.440871770] (+0.000023460) host seastar_io:io_queue_dispatched: ...
[12:57:35.441506605] (+0.000634835) host seastar_io:io_queue_completed: ...
[12:57:35.453387412] (+0.011880807) host seastar_io:io_queue_queued:
  { cpu_id = 1 }, { procname = "reactor-1" }, { fd = 44,
  req_id = 0x601000155B80, direction = 0, pclass = 0, offset = 4096,
  length = 4096 }
[12:57:35.453394076] (+0.000006664) host seastar_io:io_queue_dispatched: ...
[12:57:35.453691311] (+0.000297235) host seastar_io:io_queue_completed: ...
```

Shard 0, whose thread keeps the program's name, wrote the file in which
Shoal records its number of shards, as it started. Then each `SET`, both
of keys on shard 1, wrote a block of shard 1's log, at offsets 0 and
4,096, since each commit pads its last block, as
[chapter](#ch:files-and-storage) explained: the request waited 23
microseconds in Seastar's queue, and the disk took 635 microseconds, and
then 7 and 297. Seastar's `doc/io-tracing.md` shows more, with `perf`
and `bpftrace` too; it also makes a few mistakes, which a pull request
of ours, <https://github.com/scylladb/seastar/pull/3843>, fixes.

## Exercises

1. Add an operation to Shoal's admin API that sets a logger's level,
   such as `PUT /v1/loggers/shoal?level=debug`, and one that lists the
   loggers and their levels. What happens if the name is wrong?

2. Add a histogram of how long each command takes, from when its
   request is read until its reply is ready, for `GET` and `SET`. How
   much does keeping it cost, in instructions per request? Measure with
   `perf stat`.

3. One client sends `GET` requests for a single key, as fast as it
   can. Which of Shoal's metrics, and Seastar's, show which shard does
   the work? Why does `commands_calls`, even with
   `__aggregate__=false`, not show it?

4. Seastar's metric `reactor_utilization` is 100% minus the share of
   the last five seconds in which the shard was idle. What does a
   graph of it show, sampled every 15 seconds, when Shoal is busy one
   second in every ten? What would show it better?
