# Who uses Seastar, and why {#sol:who-uses-seastar}

## Exercise 1

We took RageDB, <https://github.com/ragedb/ragedb>, a graph database
that serves its queries over HTTP, at its commit of July 2026. About
27,600 of its 62,500 lines of C++ mention Seastar: the HTTP handlers,
the sharding layer, and the executor of queries; the parser and the
optimizer of its query language don't.

- **Coroutines or continuations?** Both, and continuations more:
  about 550 `.then()`s against about 100 `co_await`s. The project
  began in 2021, and its history shows it "trying out coroutines" that
  year, and fixing a coroutine's lifetime bug in 2026, the kind of bug
  that [chapter](#ch:object-lifetime) is about.
- **`sharded<>`?** Its graph is divided among shards, each a `Shard`,
  derived from `peering_sharded_service<Shard>`, so that a shard can
  reach the others with `container().invoke_on()`, which it does almost
  400 times.
- **Scheduling groups?** None: all its work runs in the default group.
- **Reactor backend?** It doesn't choose one, so it gets Seastar's
  default.

It's typical of the smaller users in two ways. It keeps Seastar for the
path of the data, and runs a long computation, a kind of join, on an
ordinary thread, with `std::async`, and brings the result back to the
shard with `alien::run_on()` ([chapter](#ch:threads)); and it gives
each shard a scripting language of its own, a Lua interpreter, which
runs on the shard that owns the data, as shared nothing suggests. It
also pinned Seastar to a release tag, `seastar-25.05.0`, rather than to
a commit, one of the few users to use the tags.

## Exercise 2

Seastar's Git history has about 390 changes by authors whose addresses
end with `@redpanda.com` or `@vectorized.io`, from April 2019 on:

```
$ git log --no-merges --author=@redpanda.com --author=@vectorized.io \
      --format= --name-only | ...
```

Counting the files that they changed, by directory, the most are in
the core: the public headers of `include/seastar/core/` (153 changes of
a file), `src/core/reactor.cc` (39), and the memory allocator,
`src/core/memory.cc` (25). Then come the CI's configuration
(`.github/`, 43), `perf_tests` (17 in its source alone), the Prometheus
exporter (17), the networking headers, HTTP, the testing library and
the scripts. The subjects of their commits say the same: the reactor,
`perf_tests`, the memory allocator, Prometheus and metrics, TLS. That
matches what Redpanda depends on most: a reactor and an allocator that
must not stall, benchmarks, which Redpanda writes many more of than
ScyllaDB ([chapter](#ch:testing)), and metrics.
