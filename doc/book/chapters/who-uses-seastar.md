# Who uses Seastar, and why {#ch:who-uses-seastar}

[Chapter](#ch:case-for-seastar) told Seastar's history in a few pages,
and named the programs built on it. This chapter tells more: how Seastar
began, and how its interface changed; how its three largest users,
ScyllaDB, Redpanda and Ceph's Crimson, use it, and what each added to
it, or around it; the smaller projects; what all of them found hard; and
how Seastar is developed today.

## The first years {index="Seastar!history of"}

As [chapter](#ch:case-for-seastar) told, Seastar began in 2014, at
Cloudius Systems, whose operating system for the cloud, OSv, kept
meeting applications whose own design wasted the machine. Seastar was
the other half of the answer: a framework in which an application could
use every core. Its first design already had what this book is about: a
thread per core, memory divided among the cores, futures and promises
with continuations, and an asynchronous interface to the network and
the disk. It also had what this book left out: a TCP/IP stack of its
own, which ran in the application, and reached the network card
directly through DPDK, bypassing the kernel. The memcached-compatible
server of Seastar's first public demonstration, in February 2015, is
still among its applications, in `apps/memcached`, and its native
network stack is still there too, though most of its users,
ScyllaDB among them, use the kernel's.

Cloudius, which became ScyllaDB, also took part, from 2015 to 2017, in
MIKELANGELO, a research project of the European Union's Horizon 2020
programme, about virtual machines for high-performance computing, to
which it contributed OSv and Seastar.[^mikelangelo]

[^mikelangelo]: ScyllaDB, "Project MIKELANGELO", March 8, 2016,
    <https://www.scylladb.com/2016/03/08/project-mikelangelo>.

## How the interface changed {index="Seastar!evolution"}

Seastar's interface changed more than most libraries', in three ways.

**From continuations to coroutines.** For its first five years, Seastar
code was written as chains of continuations, which C++ then had no
better way to write. In January 2019, Paweł Dziepak added support for
C++ coroutines, before they were even part of the standard, which they
became in C++20. Today coroutines are how most Seastar code is written:
in ScyllaDB's source, there are about nine `co_await`s for every
`.then()`, and Redpanda's has a similar proportion. The continuations
that remain are in older code, and in code whose speed matters enough
to avoid a coroutine's frame ([chapter](#ch:continuations)).

**From several values to one.** A future once carried any number of
values, `future<T...>`, so that `future<>` meant none, and `future<int,
sstring>` two. That generality made the code that handled futures
complicated, and few programs used more than one value; between 2019 and
2020, Seastar moved, step by step, to `future<T>`, with one value or
none.

**API levels.** Each of these changes broke programs that used the old
interface, so Seastar introduced *API levels*, which
[chapter](#ch:upgrading-seastar) explained: a program chooses the level
it was written for, and an old level keeps working for a few years
after a new one appears. There have been ten, from 2019 to 2026, and the
table in Seastar's `doc/compatibility.md` lists them. Seastar also
requires newer C++ as it goes: today it prefers C++26, and accepts
C++23.

Seastar has never had regular releases. Its repository has six tags, the
last from 2025, and its users don't use them: each takes a commit of
Seastar's main branch that it tested, as a Git submodule, as ScyllaDB
does, or in a fork of its own, as Redpanda and Ceph do, and moves to a
newer commit every few weeks or months.

## ScyllaDB {index="ScyllaDB"}

ScyllaDB is the program that Seastar was written for, and it uses all
of Seastar. It's a database compatible with Apache
Cassandra, and also with Amazon's DynamoDB, and its architecture follows
Seastar's at every level:

- **Shards own data.** Each node's data is divided among its shards, not
  only among the nodes: every partition of every table belongs to one
  shard of the node that holds it, which keeps its memtable, its
  part of the cache and its files, and serves its requests. A request
  that arrives at another shard is forwarded, with `smp`'s messages
  ([chapter](#ch:talking-between-shards)). Since 2023, ScyllaDB has also
  divided tables into [*tablets*]{index="tablet" .main}, ranges of partitions that it moves
  between nodes, and between the shards of a node, to balance them.
- **Clients go to the right shard.** ScyllaDB's drivers are
  [*shard-aware*]{index="shard-aware driver" .main} ([chapter](#ch:designing-a-sharded-application)): they
  learn which shard owns which data, and connect to each shard, through
  a port of its own, 19042, whose connections Seastar hands to the shard
  that the client's port number names, so that most requests arrive
  where their data is.
- **The cache is its own allocator.** ScyllaDB keeps its cache, and its
  memtables, in memory managed by its log-structured allocator, LSA,
  built on Seastar's ([chapter](#ch:memory)), which can move objects to
  compact memory, and evict the cache's least recently used data when
  the shard needs memory for something else; it defragments in the
  idle time of a shard ([chapter](#ch:deploying-and-tuning)).
- **Work is divided into scheduling groups.** ScyllaDB creates more than
  a dozen ([chapter](#ch:scheduling-groups)): for statements, compaction,
  streaming, memtable flushes, the commit log, gossip, and maintenance
  work such as backups, with different shares, and the maintenance
  groups in a supergroup of their own, so that background work doesn't
  starve requests, nor requests starve compaction forever.
- **Users have priorities.** ScyllaDB's [*workload prioritization*]{index="workload prioritization" .main} gives
  each [*service level*]{index="service level" .main}, a class of users that the database's
  administrator defines, a scheduling group of its own, whose shares
  decide how the CPU is divided between, say, an online application and
  an analytics job.
- **Everything else is Seastar too:** the communication between nodes is
  Seastar's RPC ([chapter](#ch:rpc)), the REST API for administration is
  Seastar's HTTP server and `json2code` ([chapter](#ch:http)), and the
  metrics are Seastar's ([chapter](#ch:observability)).

ScyllaDB and Seastar are developed together, by many of the same people:
a change that ScyllaDB needs is often made in Seastar first, and
ScyllaDB updates its copy of Seastar, a Git submodule, every week or
two.

## Redpanda {index="Redpanda"}

Redpanda is a streaming platform compatible with Apache Kafka's
interface, whose clients and tools work with it unchanged. Its founder,
Alexander Gallego, had written an RPC framework on Seastar, `smf`, in
2016, before he founded Vectorized, later Redpanda Data, in 2019, and
Redpanda is written on Seastar from top to bottom, in its own directory
tree of about fifty components, among them the Kafka protocol, the
storage, the clustering, and [*tiered storage*]{index="tiered storage" .main}, which moves old data to
object stores such as Amazon's S3.

Like ScyllaDB, Redpanda divides its data among shards: each [*partition*]{index="partition (Kafka)" .main}
of a Kafka topic belongs to one shard of each node that keeps a copy,
and each partition is replicated by its own instance of the Raft
consensus protocol, so that a node runs thousands of small Raft groups,
each on its shard, rather than one large one. A request for a partition
that arrives at another shard is forwarded to the partition's.

Redpanda builds Seastar from a fork of its own, `redpanda-data/seastar`,
which it updates from upstream from time to time: the commit that
Redpanda used in October 2026 was 83 commits ahead of Seastar's
main branch, with patches of its own, such as a CPU profiler, and 219
behind. Much of what it needed went upstream too: Redpanda's engineers
have contributed about 390 changes to Seastar since 2019, and one of
them, Travis Downs, is among those who merge others' changes. Its own
library of Seastar utilities, `ssx`, and others of its kind, which
[chapter](#ch:lessons-from-large-code-bases) looks at, hold what it
needed and didn't move upstream.

## Ceph's Crimson {index="Crimson"}

Ceph is an open-source distributed storage system, whose servers store
objects, blocks and files on clusters of thousands of disks.
[*Crimson*]{index="Crimson"} is a new implementation, on Seastar, of the server that
manages each disk, the OSD, whose classic implementation is a program
with many threads and locks. Crimson began in 2018, at Red Hat, and its
early developers contributed to Seastar what it needed: Kefu Chai, then
at Red Hat, added `alien::submit_to()` and `alien::run_on()`, through
which threads that aren't Seastar's send work to shards
([chapter](#ch:threads)), and Intel's engineers, who worked on Crimson's
network layer, added a way to choose the shard of each connection.

Crimson's code is unlike ScyllaDB's and Redpanda's in two ways. It
needed the possible errors of an operation to be part of a future's
type, checked at compile time, so it built [*errorators*]{index="errorator" .main}, futures whose
type lists the errors that they may hold; and it needed every
continuation of an operation to stop when the operation was cancelled,
so it built [*interruptible futures*]{index="interruptible future" .main}, which check a condition before every
continuation. Both reimplement parts of Seastar's internals, which is
one reason why Crimson builds Seastar from a fork of its own too, and
uses continuations as much as coroutines. And the parts of Ceph that it
didn't rewrite, such as its classic storage engine, BlueStore, run
unchanged, in ordinary threads, beside the shards. In 2026, Crimson is
still a technology preview, which Ceph's releases include, but whose
classic counterpart is what production clusters run.

## Smaller users {index="Seastar!users"}

Beyond these three, the public Seastar world is small, and most of it
is research prototypes, or projects of one person, many of them
abandoned. The more instructive ones:

| Project | What it is |
|---|---|
| RageDB | A graph database, shard per core, with Lua for queries |
| Hiactor (Alibaba) | An actor framework, the engine of GraphScope Flex's interactive queries |
| Chogori (Futurewei) | An in-memory transactional database, on a fork of Seastar with RDMA; archived |
| smf | An RPC framework with code generation, by Redpanda's founder; unmaintained |
| DeepRec (Alibaba) | TensorFlow's fork for recommendation, with a Seastar transport between servers |
| Lightbits LightOS | Commercial NVMe/TCP storage, whose data path is on Seastar |

What they have in common: they keep the shard-per-core design, but use
Seastar for the path of the data, and nothing else. Whatever must block,
or comes from a library written for threads (a parser, a scripting
language, a machine-learning framework), runs in ordinary threads
beside the shards, connected to them with `alien`'s functions or queues
of their own. Commercial users that don't publish their code are hard
to count, and are known only from their talks at conferences.

## What users find hard {index="Seastar!difficulties"}

The difficulties that these projects describe, in their documents,
issues and talks, are mostly those that this book's chapters met:

- **One Seastar per process.** A program can start Seastar only once,
  and can't restart it, which makes tests awkward, and embedding Seastar
  in a larger program harder: Crimson starts a child process to run a
  second Seastar, briefly, at startup. Seastar's developers began to
  lift the limit in 2026: the number of shards became a property of each
  instance of Seastar rather than of the process, a first step towards
  running several instances in one process.
- **Blocking calls.** A call that blocks, in the program or in a library
  that it uses, stalls a shard, and finding them takes the tools of
  [chapter](#ch:stalls).
- **Lifetimes.** References that outlive their objects, in coroutines
  and continuations ([chapter](#ch:object-lifetime)).
- **Shutdown.** Objects that must be stopped before they're destroyed,
  and abort the program when a failed test forgets
  ([chapter](#ch:clean-shutdown)).
- **The pace of change.** Seastar's interface moves, and a program that
  pins an old commit, as the smaller projects do, falls behind, until
  updating is a project of its own ([chapter](#ch:upgrading-seastar)).
- **What's missing.** Utilities that every large program needs, and
  Seastar doesn't have: [chapter](#ch:lessons-from-large-code-bases)
  looks at them.

We found no project that abandoned Seastar because of a technical
shortcoming: those that stopped gave no reason, or one about their own
people and time.

## How Seastar is developed {index="Seastar!development"}

Seastar is developed on GitHub, at <https://github.com/scylladb/seastar>,
by pull requests, which its maintainers review and merge. Avi Kivity
leads the project, and in the year before this writing merged about half
of the changes, and Pavel Emelyanov, also of ScyllaDB, a quarter; the
rest were merged by a few others, among them Redpanda's Travis Downs.
Each pull request is built and tested on several compilers, versions of
the C++ standard, build modes and reactor backends, on x86 and ARM, and
its tests run in the `debug` and `sanitize` modes as well as in
`release` ([chapter](#ch:testing)). Changes to the public interface must
keep programs that use it compiling, or be guarded by a new API level.

Most changes come from the companies that build on Seastar, which is
also why it has what they need, and lacks what nobody among them needed
yet. For a program of your own, that's the deal: you can rely on
Seastar for what ScyllaDB and Redpanda rely on, and what you need beyond
that, you may have to write, and, if you like, contribute.

## Exercises

1. Pick one of the smaller users in this chapter's table, find its source
   on GitHub, and find which of this book's tools it uses: does it use
   coroutines or continuations? `sharded<>`? Scheduling groups? Which
   reactor backend does it choose, if any?

2. In Seastar's Git history, find the changes that Redpanda's engineers
   contributed (their e-mail addresses end with `@redpanda.com`, or with
   `@vectorized.io`, from before the company was renamed). Which parts of
   Seastar did they change most?
