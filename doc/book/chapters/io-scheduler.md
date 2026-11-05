# The I/O scheduler {#ch:io-scheduler}

[Chapter](#ch:files-and-storage) measured a disk's trade-off: with more
reads in flight, it does more of them a second, but each one waits
longer. A server rarely has only one kind of I/O. Shoal's log commits
while clients wait for them; the log rewrite of a later version will
write a whole new file in the background; ScyllaDB reads for its
clients, writes its commit log, and compacts its files, all at once. If
every request went to the disk as soon as it was made, a burst of large
background writes would fill the disk's queue, and a read that a client
waits for would wait behind all of them.

So Seastar keeps a queue of its own in front of every disk, and sends
the disk only as much as it can do in a short time, the *latency goal*.
The rest waits in Seastar's queue, where Seastar, rather than the disk,
chooses what goes next: a little from each kind of work, in proportion
to the shares of its scheduling group. This is the
[*I/O scheduler*]{.index .main}. To do its job, it must know how fast
the disk is, and that's what the lines about the disk, which every
Seastar program logs when it starts, are about. This chapter explains
them, measures the disk, shows what the scheduler does with the
measurements, and shows how to cancel I/O that's no longer wanted. It
has no new version of Shoal: its log rewrite, in its own scheduling
group, comes with the rest of scheduling groups, in
[chapter](#ch:scheduling-groups).

## The lines about the disk {index="I/O scheduler!disk properties"}

[Chapter](#ch:tutorial-introduction) promised to explain these two
lines, which the programs of this book log when they start:

```
INFO  [shard 0:main] io - IO queue was unable to find a suitable maximum request length, the search was cut-off early at: 16MB
INFO  [shard 0:main] io - IO queue was unable to find a suitable maximum request length, the search was cut-off early at: 16MB
```

When a program starts, the I/O scheduler of each disk works out the
largest request that it can send the disk at once. It tries sizes that
double, starting from a block, and stops at the first one that would
take the disk longer than the latency goal; a larger request will be
split into pieces of the largest size that fits. But the scheduler knows
how long a request takes only if it's told how fast the disk is, and
without that, it assumes that the disk is infinitely fast: every size
fits, and the search stops at its limit, 16 MB, and says so, once for
writes and once for reads.

The message is harmless, but what it means isn't: the scheduler is
switched off. To an infinitely fast disk, every request can go at once,
so none ever waits in Seastar's queue, and with an empty queue there's
nothing to choose. That's fine for this book's examples, and for
a program that does little I/O. A server that reads, writes and
compacts at once needs the scheduler, and needs to tell it about its
disk.

## Measuring the disk {index="iotune"}

Seastar's program [`iotune`]{.index .main}, in its `apps/` directory,
measures a disk, and writes what it found in a YAML file:

```
iotune --evaluation-directory /var/lib/myserver --properties-file io_properties.yaml
```

It writes and reads files in the directory, for a few minutes, as hard
as it can, and records four numbers: how many reads and writes a second
the disk can do (its *IOPS*), and how many bytes a second it can read
and write (its *bandwidth*). On the laptop:

```
Measuring sequential write bandwidth: 1546 MiB/s (deviation 6%)
Measuring sequential read bandwidth: 6243 MiB/s
Measuring random write IOPS: 39626 IOPS (deviation 17%)
Measuring random read IOPS: 358333 IOPS (deviation 26%)
Writing result to io_properties.yaml
```

```
disks:
  - mountpoint: /home
    read_iops: 358333
    read_bandwidth: 6547300352
    write_iops: 39626
    write_bandwidth: 1621190784
```

The disk reads nine times as many random blocks a second as it writes,
and reads four times as many bytes. The deviations say how much the
measurements varied while `iotune` took them: a disk isn't a constant,
and `iotune` is best run on an idle machine, more than once.

A program is given the file with
[`--io-properties-file`]{index="command-line options!`--io-properties-file`"},
or the YAML itself with `--io-properties`. ScyllaDB runs `iotune` when
it's set up on a machine, in its script `scylla_io_setup`.

The file may say more about a disk than the four numbers:

- **`mountpoints:`**, a list instead of `mountpoint:`, gives several file
  systems on one disk, such as volumes of LVM, a single queue, so that
  they share the disk instead of each counting on all of it.
- **`read_saturation_length` and `write_saturation_length`** are the
  shortest requests that read or write as fast as the disk can, which
  `iotune --saturation both` measures, slowly. The scheduler splits a
  longer request into pieces of that length: a longer piece would only
  make the requests of other classes wait longer behind it.
- **`physical_block_size`** makes Seastar align every write to the
  disk's physical block, so that the disk never has to read a block in
  order to rewrite part of it. Seastar asks the disk for its block
  size; this is for a disk that reports a smaller one than it uses,
  which `iotune --detect-physical-block-size` detects, by timing
  writes of different alignments.
- **`duplex: true`** gives reads and writes a queue and a token bucket
  each, for a disk that does both at once without either slowing the
  other.

The lines about the disk don't go away. Seastar makes a queue for each
disk in the properties, and also one for all the disks that the
properties don't mention, which still knows nothing about them, and
still logs the two lines. The queue of the laptop's disk found its
maximum request length, and said nothing. With
`--logger-log-level io=debug`, each queue logs its latency goal, which
shows both:

```
DEBUG [shard 0:main] io - IO queue uses 0.75ms latency goal for /home
DEBUG [shard 0:main] io - IO queue uses 0.75ms latency goal for
INFO  [shard 0:main] io - IO queue was unable to find a suitable maximum request length, the search was cut-off early at: 16MB
INFO  [shard 0:main] io - IO queue was unable to find a suitable maximum request length, the search was cut-off early at: 16MB
```

On the laptop, `iotune` didn't want to run at all, at first:

```
ERROR [shard 0:main] iotune - Linux AIO is not supported by filesystem at /home/nyh/tmp/iotune
```

Before it measures, `iotune` checks that the file system appends to a
file without blocking the thread that asked, as XFS does; btrfs, the
laptop's file system, blocks. For a Seastar server that's a reason to
use XFS, as ScyllaDB requires. To measure the laptop's disk anyway, this
chapter used a copy of `iotune` without the check, in a directory where
btrfs doesn't compress (as [chapter](#ch:files-and-storage) explained,
compression would measure the file system instead of the disk).

## How the scheduler decides {index="I/O scheduler!token bucket"}

With the four numbers, the scheduler can tell what a request costs: the
share of a second of the disk's time it takes. A read of $b$ bytes
costs

$$\frac{1}{\textit{read IOPS}} + \frac{b}{\textit{read bandwidth}}$$

and a write the same, with the write numbers. Seastar's own
documentation (`doc/io-scheduler.md`) shows that the disk is busy but
not overloaded when the costs of the requests it's given add up to at
most one second a second, whatever the mix of reads and writes, small
and large. On the laptop, a read of 4 KB costs 3.4 µs, most of it the
read itself (1/358,333 of a second) rather than its bytes, and a write
of 128 KB costs 106 µs, three quarters of it its bytes.

The scheduler enforces that with a [*token bucket*]{.index .main}.
Tokens flow into the bucket at the rate of one second's worth a second.
A request is sent to the disk only when the bucket holds the tokens it
costs, which it takes; otherwise, it waits in Seastar's queue. The
bucket holds at most the tokens of the latency goal, so after a quiet
moment, the disk is sent at most that much at once: whatever is sent
together, the disk finishes in about the latency goal. The disk is
shared by all the shards, and so is the bucket: by default, one for
the shards of each NUMA node (`--num-io-groups` sets the number of such
groups). A shard doesn't wait for the tokens to be there before it
takes them: it reserves them, which may put the bucket in debt, and
sends the request when the bucket's income has paid for its place in
line. So the shards' requests go to the disk in the order in which they
were reserved, as customers who take a numbered ticket are served in
order, and no shard can take the tokens that another is waiting for. The goal is
[`--io-latency-goal-ms`]{index="command-line options!`--io-latency-goal-ms`"},
and by default one and a half times the task quota, which
[chapter](#ch:how-seastar-runs-your-code) explained: 0.75 ms. (So the
largest request that the scheduler sends the disk at once is the
largest that the disk does in 0.75 ms; that's the "maximum request
length" of the lines about the disk.)

The scheduler also watches whether the disk keeps up. If the disk
completes requests more slowly than they're sent, by more than 10%
(`--io-flow-rate-threshold`, 1.1), the disk is slower than its
properties said, and the scheduler counts every request as costing
more, by the ratio of the two rates, so that it sends less, until the
disk catches up. The rate that really matters, at which the disk does
its work, can't be seen: Seastar learns that a request is done only when
it next polls for completions. But the requests that are done and not
yet reported can't pile up without end, since Seastar polls often, so
over a while the completions keep pace with the disk's work, and their
rate stands for it. Every 100 latency goals, 75 ms by default, the
scheduler divides the requests that it sent by those completed, and
averages the ratio with the earlier ones, the new one weighing 5%, so
that one slow moment doesn't throttle the disk.
`iotune` measures a disk at its best, and a disk that's busy with
something else, or whose cache is full, is slower.

What's in the queue is ordered by fair queueing between the *classes*
of I/O. A request's class is the scheduling group of the fiber that
made it, the same groups that divide the CPU, with the same shares, as
[chapter](#ch:scheduling-groups) explains. When requests of two groups
wait, each gets a part of the disk's capacity in proportion to its
shares; a group that has nothing to do leaves its part to the others.

## Reads and writes, together {index="I/O scheduler!shares"}

This program, `reads-and-writes.cc`, reads 4 KB at random places, with
as many reads in flight as its option `--reads-in-flight` says (one,
by default), for ten seconds, and measures how many it did, and how long
each took:

```{.cpp include="examples/io-scheduler/reads-and-writes.cc" snippet="reader"}
```

Then it does the same while another fiber writes over another file, as
fast as it can, with the block size and the number of writes in flight
that `--write-size` and `--writes-in-flight` say (128 KB, and 16, by
default):

```{.cpp include="examples/io-scheduler/reads-and-writes.cc" snippet="writer"}
```

The reads and the writes are in scheduling groups of their own, with
the shares that the options `--read-shares` and `--write-shares` give
them, 1,000 each by default.
[`create_scheduling_group()`]{index="`seastar::create_scheduling_group()`" .main},
from `<seastar/core/scheduling.hh>`, makes a group, and
[`with_scheduling_group()`]{index="`seastar::with_scheduling_group()`" .main},
from `<seastar/core/with_scheduling_group.hh>`, runs a function in it,
and all the continuations that follow:

```{.cpp include="examples/io-scheduler/reads-and-writes.cc" snippet="groups"}
```

```{.cpp include="examples/io-scheduler/reads-and-writes.cc" snippet="together"}
```

With its defaults, one read at a time beside the writes, the program
found little for the scheduler to do. Alone, a read took 0.050 ms on
average; beside the writes, 0.072 ms, and 0.17 ms for the slowest 1%,
with the disk's properties or without them, and whatever the shares.
The laptop's disk, an NVMe SSD, has many queues of its own, and serves
a read beside writes well, as long as the writes don't use it all up.

So the program was run harder: 64 reads in flight, and 64 writes of a
megabyte, each group trying to use the whole disk. Without the disk's
properties, and then with them, each with equal shares, and with the
writes' shares lowered to 100 (the best of three runs of five seconds
each):

| Properties | Shares (reads:writes) | Reads a second | Slowest 1% | Writes |
|---|---|--:|--:|--:|
| none | 1000:1000 | 167,542 | 0.73 ms | 1,149 MB/s |
| none | 1000:100 | 172,740 | 0.73 ms | 1,106 MB/s |
| `iotune`'s | 1000:1000 | 145,113 | 1.23 ms | 789 MB/s |
| `iotune`'s | 1000:100 | 262,632 | 0.99 ms | 170 MB/s |

Without the properties, the shares changed nothing: every request went
to the disk at once, and the disk, not Seastar, decided which went
first. With the properties, the shares divided the disk. The costs of
the previous section tell how: with equal shares, the reads used
145,113 × 3.4 µs, half of each second of the disk's time, and the
writes, 752 writes of a megabyte a second, at 672 µs each, the other
half. With the writes' shares lowered to a tenth of the reads', the
reads got 90% of the disk's time, and the writes 11%: not quite ten to
one, but close.

The scheduler isn't free. With its properties, the disk did less in
all than without them: `iotune` measures each kind of request alone,
and this disk does a mix of reads and writes faster than its parts
suggest, so the scheduler, which believes the parts, leaves some of
the disk unused. And the slowest reads waited longer, since they now
waited in Seastar's queue, for tokens, rather than in the disk's. What
the scheduler buys is control: here, the reads got the disk when they
needed it, whatever the writes did, in the proportion that the shares
asked for. A server whose reads are waited for by clients, and whose
writes are background work, wants exactly that.

## Cancelling I/O {index="`seastar::io_intent`"}

A request that waits in Seastar's queue may stop being wanted before
it's sent: the client that asked for the data hung up, or the read was
one of several, and another one already answered. An
[`io_intent`]{index="`seastar::io_intent`" .main}, from
`<seastar/core/io_intent.hh>`, lets a program take such requests back.
Every read and write takes a pointer to one, as its last argument; the
intent's `cancel()` removes all the requests made with it that are still
in the queue, whose futures fail with `cancelled_error`. Requests already
sent to the disk can't be taken back, and complete as usual. This
program starts a hundred reads at once, with one intent, and cancels
them a millisecond later:

```{.cpp include="examples/io-scheduler/cancel.cc" snippet="cancel"}
```

Each of these reads costs 163 µs, so the millisecond, with the latency
goal's worth that the bucket held when the reads began, pays for about
ten of them. Without the disk's properties, the scheduler sent all of the
reads to the disk at once, and there was nothing left to cancel; with
them, it sent only what the disk could do in its latency goal, and kept
the rest:

```
without the properties:  100 reads: 100 done, 0 cancelled
with them:               100 reads: 10 done, 90 cancelled
```

Destroying an intent cancels its requests too, so an intent must live
as long as its requests are wanted: here, until every read is done or
cancelled.

## Watching the scheduler

The scheduler reports what it does in metrics, which
[chapter](#ch:observability) shows how to collect, for each disk, shard
and class: `io_queue_queue_length`, the requests waiting in Seastar's
queue, and `io_queue_disk_queue_length`, those sent to the disk;
`io_queue_delay`, how long the latest request waited in the queue;
`io_queue_total_read_ops`, `io_queue_total_write_ops` and their bytes;
`io_queue_total_split_ops`, the requests that were split because they
were larger than the maximum length; and `io_queue_starvation_time_sec`,
how long a class had requests waiting while none of its requests were
at the disk. A Seastar built with LTTng (`cmake
-DSeastar_LTTNG=ON`) also has tracepoints for every request, which
Seastar's `doc/io-tracing.md` explains.

To try the scheduler without writing a program,
[`io_tester`]{.index .main}, in Seastar's `apps/` directory, runs
workloads that a YAML file describes: reads and writes, random or
sequential, of any size and in any number, each in its own scheduling
group with its shares, on as many shards as asked, and reports their
throughput and latencies. Its documentation is `doc/io-tester.md`.

## Exercises

1. Run `reads-and-writes.cc` as the table's third row did, with the
   disk's properties and 64 reads and 64 writes in flight, and with
   `--io-latency-goal-ms` of 0.25, 2 and 5. How do the slowest reads,
   and the writes, change? Why?

2. Run it with the writes' shares at 300 and at 30, and work out, from
   the costs of each read and write, how the disk's time was divided.
   How close is it to the shares?

3. Edit the properties to claim that the disk writes only a quarter as
   fast as `iotune` measured (both write numbers), and run the program
   with its defaults. What happens to the writes, and to the reads?
