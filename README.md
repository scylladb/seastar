Seastar
=======

[![Test](https://github.com/scylladb/seastar/actions/workflows/tests.yaml/badge.svg)](https://github.com/scylladb/seastar/actions/workflows/tests.yaml)
[![Version](https://img.shields.io/github/tag/scylladb/seastar.svg?label=version&colorB=green)](https://github.com/scylladb/seastar/releases)
[![License: Apache2](https://img.shields.io/github/license/scylladb/seastar.svg)](https://github.com/scylladb/seastar/blob/master/LICENSE)
[![n00b issues](https://img.shields.io/github/issues/scylladb/seastar/n00b.svg?colorB=green)](https://github.com/scylladb/seastar/labels/n00b)

Seastar is a C++ framework for writing high-performance server applications
on modern multi-core hardware. It powers [ScyllaDB](https://github.com/scylladb/scylladb),
[Redpanda](https://www.redpanda.com/), [Ceph Crimson](https://github.com/ceph/ceph)
and other demanding systems.

Seastar is written in modern C++, and supports C++23 and C++26.

To build Seastar and use it in your own project, see [BUILD.md](./BUILD.md).

Philosophy
----------

Server frameworks have traditionally forced a choice between efficiency
and the ability to build complex applications. Seastar aims for both: it
lets you write large, complex servers while extracting the full
performance of the hardware. Three principles make this possible:

* **Composable primitives.** Everything that can take time is represented
  by the same abstraction, a *future*: reading from the network, writing to
  disk, waiting on a timer, running a function on another core, or even a
  long CPU-bound computation. These operations compose freely. A single
  coroutine can read a request from a socket, fan out to several disks and
  cores in parallel, wait for the results, and reply, all written as
  straight-line code.

* **Move operations to the data, not data to the operations.** Seastar
  runs one thread per core (a *shard*) and partitions memory between them.
  Each piece of data is owned by a single shard; rather than locking the
  data and touching it from whichever core happens to need it, you send the
  operation to the shard that owns the data. There are no locks, no
  contended atomics and no cache-line bouncing on the fast path, so
  performance scales with the number of cores.

* **Symmetric architecture.** All shards have the same role. There are no
  dedicated network threads, I/O threads or worker pools; every shard runs
  the full stack (networking, storage, application logic, and a
  cooperative scheduler) for its own slice of the data. Adding cores adds
  capacity uniformly, and the whole application is built from one kind of
  component, replicated.

Features
--------

* **Futures, promises and coroutines**: a complete asynchronous programming
  model. C++ coroutines (`co_await`) are the recommended way to write new
  code, and interoperate seamlessly with future-returning functions.
* **Sharded services**: `seastar::sharded<T>` instantiates a service on
  every core and provides tools to invoke methods on one, some, or all
  shards, and to map-reduce across them.
* **Concurrency primitives**: semaphores, gates, condition variables,
  pipes, queues, abort sources, parallel loops (`parallel_for_each`,
  `max_concurrent_for_each`) and fork/join (`coroutine::all`).
* **CPU scheduler**: scheduling groups with configurable shares isolate
  application components from each other.
* **Disk I/O scheduler**: DMA-based, zero-copy file I/O using `io_uring`
  or Linux AIO, with a scheduler that models the disk's capabilities (as
  measured by `iotune`) to keep latency low while maximizing throughput.
* **Networking**: TCP, UDP, SCTP and Unix domain sockets on top of the
  POSIX stack, or an optional [native userspace TCP/IP stack](doc/native-stack.md)
  running over [DPDK](doc/building-dpdk.md) for kernel bypass. TLS is
  supported.
* **Protocols**: HTTP server and client, WebSocket, [RPC](doc/rpc.md)
  with streaming and compression, DNS resolution, and JSON.
* **Memory management**: a per-shard memory allocator that avoids
  cross-core contention, with foreign pointers for safely passing
  ownership between shards.
* **Observability**: [Prometheus](doc/prometheus.md) metrics, a
  structured logger, a reactor stall detector with backtraces, and
  [I/O tracing](doc/io-tracing.md).
* **Testing**: `SEASTAR_TEST_CASE` integration with Boost.Test for
  writing asynchronous unit tests, a microbenchmark framework, and
  allocation failure injection.

Examples
--------

### A sharded echo server

This complete program starts an echo server on every core. Each shard
listens on the same port, accepts its own connections, and serves each one
concurrently in its own lightweight fiber.

```cpp
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/gate.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/sharded.hh>
#include <seastar/core/signal.hh>
#include <seastar/net/api.hh>

class echo_server {
    seastar::server_socket _listener;
    seastar::gate _gate;
public:
    seastar::future<> start(uint16_t port) {
        seastar::listen_options lo;
        lo.reuse_address = true;
        _listener = seastar::listen(seastar::make_ipv4_address({port}), lo);
        (void)seastar::try_with_gate(_gate, [this] { return accept_loop(); });
        co_return;
    }

    seastar::future<> stop() {
        _listener.abort_accept();
        co_await _gate.close();  // wait for all connections to finish
    }

private:
    seastar::future<> accept_loop() {
        try {
            while (true) {
                auto [conn, addr] = co_await _listener.accept();
                // Serve each connection concurrently, in its own fiber.
                (void)seastar::try_with_gate(_gate, [this, conn = std::move(conn)] mutable {
                    return handle(std::move(conn));
                });
            }
        } catch (...) {
            // stop() called abort_accept()
        }
    }

    seastar::future<> handle(seastar::connected_socket conn) {
        auto in = conn.input();
        auto out = conn.output();
        while (auto buf = co_await in.read()) {
            co_await out.write(std::move(buf));
            co_await out.flush();
        }
        co_await out.close();
    }
};

int main(int argc, char** argv) {
    seastar::app_template app;
    return app.run(argc, argv, [] () -> seastar::future<> {
        seastar::sharded<echo_server> server;
        co_await server.start();  // construct an echo_server on every shard
        co_await server.invoke_on_all(&echo_server::start, uint16_t(1234));

        seastar::promise<> stop_requested;
        seastar::handle_signal(SIGINT, [&] { stop_requested.set_value(); }, true);
        co_await stop_requested.get_future();

        co_await server.stop();  // calls echo_server::stop() on every shard
    });
}
```

### Concurrency within a coroutine

`co_await` runs operations one after another. To overlap independent
operations, fork and join them:

```cpp
#include <seastar/core/seastar.hh>
#include <seastar/coroutine/all.hh>
#include <seastar/coroutine/parallel_for_each.hh>

seastar::future<int> fetch(int key);

seastar::future<int> sum_two(int k1, int k2) {
    // Both fetches run concurrently.
    auto [a, b] = co_await seastar::coroutine::all(
        [&] { return fetch(k1); },
        [&] { return fetch(k2); });
    co_return a + b;
}

seastar::future<uint64_t> total_size(std::vector<seastar::sstring> names) {
    uint64_t total = 0;
    // Stat all the files concurrently.
    co_await seastar::coroutine::parallel_for_each(names, [&] (const seastar::sstring& name) -> seastar::future<> {
        total += co_await seastar::file_size(name);
    });
    co_return total;
}
```

### Talking to other shards

Shards share no state; they communicate by sending work to each other.

```cpp
#include <seastar/core/sharded.hh>

class counter {
    uint64_t _hits = 0;
public:
    void hit() { ++_hits; }
    uint64_t hits() const { return _hits; }
    seastar::future<> stop() { co_return; }
};

// Route each key to the shard that owns it.
seastar::future<> record(seastar::sharded<counter>& c, uint64_t key) {
    co_await c.invoke_on(key % seastar::this_smp_shard_count(), [] (counter& local) {
        local.hit();
    });
}

// Gather a value from every shard.
seastar::future<uint64_t> total_hits(seastar::sharded<counter>& c) {
    co_return co_await c.map_reduce0(
        [] (const counter& local) { return local.hits(); },
        uint64_t(0), std::plus<>());
}
```

### Isolating background work

Scheduling groups divide CPU and I/O between application components, and
semaphores bound how much concurrency any one of them uses.

```cpp
#include <seastar/core/scheduling.hh>
#include <seastar/core/semaphore.hh>
#include <seastar/coroutine/parallel_for_each.hh>
#include <seastar/coroutine/switch_to.hh>

seastar::future<> compact(table& t);

seastar::future<> compact_all(seastar::scheduling_group background, std::vector<table*> tables) {
    // From here on, this coroutine runs with the background group's shares.
    co_await seastar::coroutine::switch_to(background);

    seastar::semaphore limit(4);  // at most 4 compactions at a time
    co_await seastar::coroutine::parallel_for_each(tables, [&] (table* t) -> seastar::future<> {
        auto units = co_await seastar::get_units(limit, 1);
        co_await compact(*t);
    });
}

// Somewhere during startup:
//   auto background = co_await seastar::create_scheduling_group("compaction", 100);
```

Getting started
---------------

* [BUILD.md](./BUILD.md) explains how to build Seastar and link it into
  your application.
* The [tutorial](doc/tutorial.md) is a comprehensive introduction to
  Seastar programming; there is also a shorter [mini tutorial](doc/mini-tutorial.md).
* The API reference and other documentation are available on the
  [web](https://docs.seastar.io/master/index.html).
* The [demos](demos/) directory contains small example programs.
* [HACKING.md](./HACKING.md) and [CONTRIBUTING.md](./CONTRIBUTING.md) are
  for those working on Seastar itself.

Recommended hardware configuration
----------------------------------

* CPUs - As many as you need. Seastar is well suited to multicore and NUMA systems.
* NICs - As fast as possible. The more hardware queues per CPU, the better
  for Seastar; otherwise we have to emulate that in software.
* Disks - Fast NVMe SSDs with a high number of IOPS.
* Client machines - Usually a single client machine cannot fully load a
  Seastar server. We recommend running load generators on machines other
  than the server, and using several of them.

Resources
---------

* Seastar Development Mailing List: Discuss challenges, propose improvements, send
  code contributions (patches), and get help from experienced developers.
  Subscribe or browse archives: [here](https://groups.google.com/g/seastar-dev)
  (or email seastar-dev@googlegroups.com).
* GitHub Discussions: For more casual conversations and quick questions, consider
  using the Seastar project's [discussions on GitHub](https://github.com/scylladb/seastar/discussions).
* Issue Tracker: File bug reports on the project's [issue tracker](https://github.com/scylladb/seastar/issues).

Learn more about Seastar on the main [project website](https://seastar.io/).

Projects using Seastar
----------------------

* [ScyllaDB](https://github.com/scylladb/scylladb): A fast and reliable NoSQL data store compatible with Cassandra and DynamoDB
* [Redpanda](https://www.redpanda.com/): A Kafka-compatible streaming data platform for mission-critical systems
* [Ceph - Crimson](https://github.com/ceph/ceph): Next-generation OSD (Object Storage Daemon) implementation based on the Seastar framework
* [cpv-cql-driver](https://github.com/cpv-project/cpv-cql-driver): C++ driver for Cassandra/Scylla based on the Seastar framework
* [cpv-framework](https://github.com/cpv-project/cpv-framework): A web framework written in C++ based on the Seastar framework
* [smf](https://github.com/smfrpc/smf): The fastest RPC in the West
