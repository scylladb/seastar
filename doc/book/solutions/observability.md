# Observability {#sol:observability}

## Exercise 1

The patch, `examples/solutions/shoal-loggers.patch`, adds two operations
to `admin.json`, `GET /v1/loggers` and `PUT /v1/loggers/{name}`, with a
model of a logger's name and level. The handlers use the registry of all
the loggers, which `seastar::logger`'s constructor fills:

```{.cpp include="examples/solutions/shoal-loggers.patch" file="admin.cc" snippet="loggers"}
```

A level is parsed with `log_level`'s `operator>>`, which accepts the
names that `--logger-log-level` does, and fails on any other, which the
handler answers with 400 Bad Request. A name that isn't a logger's
makes the registry throw `std::out_of_range`, which the handler turns
into 404 Not Found; left alone, it would have been 500 Internal Server
Error, as [chapter](#ch:http) explained. The handler sets the level on
the shard that serves the request only, and that's enough: a logger is
one object, which all the shards share, and its level is an atomic
variable.

```
$ curl -X PUT 'localhost:8080/v1/loggers/shoal?level=debug'
$ curl -X PUT 'localhost:8080/v1/loggers/shaol?level=debug'
{"message": "no logger 'shaol'", "code": 404}
$ curl -X PUT 'localhost:8080/v1/loggers/shoal?level=loud'
{"message": "unknown level 'loud'", "code": 400}
$ curl localhost:8080/v1/loggers
[{"name": "dns_resolver", "level": "info"},{"name": "httpd", "level": "info"},
{"name": "io", "level": "info"},{"name": "seastar_memory", "level": "info"},
{"name": "tls", "level": "info"},{"name": "exception", "level": "info"},
{"name": "seastar", "level": "info"},{"name": "shoal", "level": "debug"}]
```

The list shows Seastar's own loggers too, and after the first `PUT`,
Shoal logged its debug messages, such as each client's connection.

## Exercise 2

The patch, `examples/solutions/shoal-latency.patch`, keeps a
`latency_histogram` for `GET` and one for `SET` in each shard's store,
and `serve()` notes the time when it reads a request, and adds the time
since then to the command's histogram when the reply is ready:

```{.cpp include="examples/solutions/shoal-latency.patch" file="main.cc" snippet="timed"}
```

A reply executed on the connection's shard is usually ready at once, and
then costs no continuation. Each histogram is a metric with a label,
`command`, as the counters of calls are, but reported for each shard:

```{.cpp include="examples/solutions/shoal-latency.patch" file="commands.cc" snippet="latency-metrics"}
```

The histograms cost more than their few increments. We measured Shoal
on one shard, with a client that sent pipelined `SET` and `GET`
requests, four million of each, with `perf stat`, in three runs of each
version; in the best runs, a request took 3,519 instructions and 1,084
cycles without the histograms, and 3,775 instructions and 1,266 cycles
with them: 7% more instructions, 17% more cycles, and 8% fewer `GET`s a
second. Most of it is the clock: `std::chrono::steady_clock::now()`,
which the request reads twice, took 80 instructions and 53 cycles a
call on the same machine, 11.6 ns, measured in a loop of 20 million
calls. A server that measures every request's latency pays for it; one
that needs only an estimate could time one request in ten, or a
hundred.

## Exercise 3

We ran Shoal with two shards, set one key, and had one client send
`GET`s of it for eight seconds. The key was on shard 1, and the
client's connection on shard 0. Then, with `__aggregate__=false`, the
metrics said:

```
shoal_commands_calls{command="get",shard="0"} 739514
shoal_reactor_utilization{shard="0"} 49.777994
shoal_reactor_utilization{shard="1"} 9.991818
shoal_reactor_tasks_processed{shard="0"} 8155694
shoal_reactor_tasks_processed{shard="1"} 741465
```

`commands_calls` counts a call where its request was read, on the shard
of the connection, which then sends it to the key's shard, so it says
which shards the clients talk to, not which shards do the work. Seastar's
own metrics show that: shard 1 was busy 10% of the time, and ran one
task for each `GET`, while shard 0 read the requests, sent them away,
and wrote the replies. To see which shards execute the commands, Shoal
would have to count them where they're executed, in `execute_here()`,
too.

## Exercise 4

Each second, the reactor measures the share of the second in which the
shard was idle, and `reactor_utilization` is 100% minus the average of
the last five of these. A sample taken every 15 seconds therefore sees
only the last 5 seconds of each 15, and whether they held the busy
second is a matter of luck. We loaded Shoal for one second in every ten,
and sampled it every 15 seconds:

```
 15s: utilization  19.8%, rate of cpu_busy_ms over the last 15 s  13.1%
 30s: utilization   0.0%, rate of cpu_busy_ms over the last 15 s   6.9%
 45s: utilization  19.7%, rate of cpu_busy_ms over the last 15 s  12.8%
 60s: utilization   0.0%, rate of cpu_busy_ms over the last 15 s   7.2%
```

The graph of `reactor_utilization` would show a shard that is idle half
the time and busy a fifth of it the other half. The counter
`reactor_cpu_busy_ms` adds up every millisecond in which the shard was
busy, whenever it's sampled, so its rate over an interval is the true
share of that interval: here 13% or 7%, as the interval held two busy
seconds or one, and 10% over any minute. A counter's rate is the better
measure for a monitoring system, and that's what Prometheus's `rate()`
computes from it; a gauge that averages is for a human who looks now.
