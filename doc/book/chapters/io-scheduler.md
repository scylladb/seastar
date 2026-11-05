# The I/O scheduler {#ch:io-scheduler}

[Chapter](#ch:files-and-storage) measured a disk's trade-off: with more
reads in flight, it does more of them a second, but each one waits
longer, and beyond some point, more requests in flight buy nothing but
waiting, since a request waits for those ahead of it. Linux accepts far
more requests than that: on the laptop of
[chapter](#ch:tutorial-introduction), it queues up to 1,023 for each of
the disk's hardware queues (the file
`/sys/block/nvme0n1/queue/nr_requests` says so). And once a request is
in that queue, nothing can move it ahead or take it back: the disk
works through what it was given in an order of its own, knowing nothing
about which requests someone is waiting for.

That matters, because a server often has several kinds of disk work
going on at the same time: some that clients wait for, and some in the
background. ScyllaDB, for example, reads the data that its clients ask
for, writes its commit log, and compacts its files, all at the same
time. If every request went to the disk as soon as it was made, a burst
of large background writes would fill the disk's queue, and a read that
a client waits for would wait behind them
([Figure](#fig:disk-queues)). Shoal will soon have the same problem.
Its log grows forever, so Shoal version 11 will add a *log rewrite*,
which writes a new, compact log in the background, while clients wait
for their changes to be committed to the current one.

![A read (R) that a client waits for, and background writes (W), without
and with Seastar's queue in front of the disk.](figures/disk-queues.svg){#fig:disk-queues placement="h"}

So Seastar keeps a queue of its own in front of every disk, and sends
the disk only as much as it can do in a short time, the *latency goal*:
by default 0.75 ms, one and a half times the task quota of
[chapter](#ch:how-seastar-runs-your-code), the time for which the
reactor runs tasks before it polls again.
The rest waits in Seastar's queue, where Seastar, rather than the disk,
chooses what goes next. To choose, it must know what kind of work each
request is. A program puts each kind of work in a
[*scheduling group*]{index="scheduling group"} of its own, such as one
for the reads that clients wait for and one for background writes, and
gives each group a number of *shares*. Every request belongs to the
group of the fiber that made it, and Seastar sends each group's
requests to the disk in proportion to its shares; a group with nothing
to do leaves its part to the others. (Scheduling groups divide the CPU
too, in the same proportions, as [chapter](#ch:case-for-seastar)
mentioned and [chapter](#ch:scheduling-groups) will explain.)

A read that a client waits for can then go to the disk ahead of writes
that were made before it, and in the disk, it waits only for the little
that's already there. This doesn't make the disk any faster: the
requests still wait, only in Seastar's queue instead of the disk's.
What Seastar's queue buys is control over which of them waits: it
divides the disk between the groups as their shares ask, and it can
also cancel a request that hasn't been sent yet. This is the
[*I/O scheduler*]{.index .main}.

This chapter shows what the scheduler needs to know about the disk and
how to measure it, then what the scheduler does with the measurements,
how to cap a group's bandwidth, and last, how to cancel I/O that's no
longer wanted. It has no new version of
Shoal: version 11, whose log rewrite runs in a scheduling group of its
own, comes in the next chapter, [chapter](#ch:scheduling-groups), with
the rest of scheduling groups.

## What Seastar needs to know about the disk {index="I/O scheduler!disk properties"}

The I/O scheduler, to do its job, must know the disk's capacity: how
many reads and writes it can do a second, and how many bytes a second it can read and write. From these four rates, the
scheduler works out how much the disk can do in the latency goal, which
is how much it lets be in flight: the limit is in the disk's time, not
in a number of requests. But Seastar doesn't measure these rates when a
program starts: the program must be given them, with options that the
next section shows, and this book's programs, run without those
options, weren't.

[Chapter](#ch:tutorial-introduction) promised to explain two lines that
the programs of this book log when they start. They're about these
rates, which the programs weren't given:

```
INFO  [shard 0:main] io - IO queue was unable to find a suitable maximum request length, the search was cut-off early at: 16MB
INFO  [shard 0:main] io - IO queue was unable to find a suitable maximum request length, the search was cut-off early at: 16MB
```

When a program starts, the I/O scheduler of each disk works out its
*maximum request length*: the largest request that the disk can do
within the latency goal. A larger request would keep the disk busy for
longer than the goal allows, so Seastar splits it into pieces of that
length. To find it, the scheduler computes, from the four rates, how
long a request of 512 bytes takes, then of 1 KB, 2 KB, and so on,
doubling, and stops at the first that takes longer than the latency
goal; the length before it is the maximum. When it wasn't given the
rates, the scheduler takes the disk to be infinitely fast: every request takes no
time, so the doubling never finds one that's too long. It gives up at
16 MB, the longest request that Seastar allows, and logs the message
above, once for writes and once for reads.

The message is harmless, but what it means isn't: the scheduler is
switched off. To an infinitely fast disk, every request can go immediately,
so none ever waits in Seastar's queue, and with an empty queue there's
nothing to choose. That's fine for this book's examples, and for
a program that does little I/O. A server that reads, writes and
compacts at the same time needs the scheduler, and needs to tell it about its
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

- **`mountpoints:`**, a list instead of `mountpoint:`, gives a single
  queue to several file systems on one disk, such as volumes of LVM, so
  that they share the disk instead of each counting on all of it.
- **`read_saturation_length` and `write_saturation_length`** are the
  shortest requests that read or write as fast as the disk can, which
  `iotune --saturation both` measures, slowly. A file's
  `disk_read_max_length()` and `disk_write_max_length()` then recommend
  them as the longest requests, and Seastar's file streams cap their
  buffers at them: a longer request would read or write no faster, and
  only make the requests of other classes wait longer behind it.
- **`physical_block_size`** makes Seastar align every write to the
  disk's physical block, so that the disk never has to read a block in
  order to rewrite part of it. Seastar asks the disk for its block
  size; this is for a disk that reports a smaller one than it uses,
  which `iotune --detect-physical-block-size` detects, by timing
  writes of different alignments.
- **`duplex: true`** gives reads and writes a queue and a token bucket
  each, for a disk that does both at the same time without either slowing the
  other.

The two messages about the disk don't go away. Seastar makes a queue for each
disk in the properties, and also one for all the disks that the
properties don't mention, which still knows nothing about them, and
still logs the two lines. The queue of the laptop's disk found its
maximum request length, and said nothing. With
`--logger-log-level io=debug`, each queue logs its latency goal, which
shows both queues:

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

Before it measures, `iotune` checks that the disk's file system really
does writes asynchronously. It writes a thousand blocks to a file of its
own, with Linux AIO, and counts how often the call that only submits a
write had to wait for it instead; more than once in ten writes fails
the check. XFS and ext4 pass it. btrfs, the laptop's file system, fails
it on every write: as "File systems" in
[chapter](#ch:files-and-storage) explained, btrfs does direct writes
synchronously, and a Seastar program avoids waiting for them only
because every write takes a detour through another thread, which costs
time and CPU. That's why that chapter recommended
XFS or ext4 for a Seastar server's data. To measure the laptop's disk
anyway, this
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
moment, the disk is sent at most that much at a time: whatever is sent
together, the disk finishes in about the latency goal, which a program
can change with
[`--io-latency-goal-ms`]{index="command-line options!`--io-latency-goal-ms`"}.
This is also why no request may be longer than the maximum request
length: a longer one would cost more tokens than the bucket can hold.

The disk is shared by all the shards, and so is the bucket: by default,
one for the shards of each NUMA node (`--num-io-groups` sets the number
of such groups). A shard doesn't wait for the tokens to be there before
it takes them: it reserves them, which may put the bucket in debt, and
sends the request when the bucket's income has paid for its place in
line. So the shards' requests go to the disk in the order in which they
were reserved, as customers who take a numbered ticket are served in
order, and no shard can take the tokens that another is waiting for.

This is the kind of sharing that [chapter](#ch:case-for-seastar) warned
about. The bucket is a few counters in memory, which the shards update
with atomic operations, so the cache line that holds them moves from
core to core. Seastar accepts this because the alternatives were worse.
Until 2020, a few shards were the disk's *coordinators*: they sent the
requests to the disk, and the other shards passed their requests to
them as messages. A coordinator could be overloaded, the other shards'
requests waited for the messages, and canceling a request needed
another message. Dividing the disk's capacity among the shards in fixed
parts, which shares nothing, would leave the part of a shard that has
no I/O to do unused, while other shards wait. Seastar also keeps the
cost of the sharing small. A shard doesn't reserve tokens for each
request: it reserves them in batches, each at least the shard's part of
the latency goal (with 8 shards, an eighth of it). So when all the
shards are busy, the cache line moves a few times for each shard in a
latency goal, which costs a few microseconds in 0.75 ms. And because a
group is one NUMA node's, the line doesn't move between the
processors of a machine that has several.

The scheduler also watches whether the disk keeps up, since a disk can
be slower than its properties say: `iotune` measures a disk at its
best, and a disk that's busy with something else, or whose cache is
full, is slower. If the disk completes requests more slowly than
they're sent, by more than 10% (`--io-flow-rate-threshold`, 1.1), the
scheduler counts every request as costing more, by the ratio of the two
rates, so that it sends less, until the disk catches up. Every 100
latency goals, 75 ms by default, the scheduler divides the requests that
it sent in that time by those completed, and averages the ratio with the
earlier ones, the new one weighing 5%, so that one slow moment doesn't
throttle the disk.

The scheduler can't measure exactly how fast the disk works: it learns
that a request is done only when it next polls for completions, a little
after the disk finished it. So it measures how fast it sees requests
complete instead. Over 75 ms, the difference doesn't matter: Seastar
polls often, so it sees each completion soon after it happens.

What's in the queue is ordered by fair queueing between the *classes*
of I/O. A request's class is the scheduling group of the fiber that
made it, the same groups that divide the CPU, with the same shares, as
[chapter](#ch:scheduling-groups) will explain. When requests of two groups
wait, each gets a part of the disk's capacity in proportion to its
shares; a group that has nothing to do leaves its part to the others.

## Reads and writes, together {index="I/O scheduler!shares"}

This program, `reads-and-writes.cc`, reads 4 KB at random offsets, with
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
them, 1,000 each by default. Scheduling groups are the subject of the
next chapter, [chapter](#ch:scheduling-groups), which explains them in
full; this program uses only the two functions it needs to put each
kind of I/O in a class of its own.
[`create_scheduling_group()`]{index="`seastar::create_scheduling_group()`"},
from `<seastar/core/scheduling.hh>`, makes a group with a name and its
shares, and
[`with_scheduling_group()`]{index="`seastar::with_scheduling_group()`"},
from `<seastar/core/with_scheduling_group.hh>`, runs a function in a
group, so that the I/O the function does is in that group's class:

```{.cpp include="examples/io-scheduler/reads-and-writes.cc" snippet="groups"}
```

```{.cpp include="examples/io-scheduler/reads-and-writes.cc" snippet="together"}
```

The program always puts the reads and the writes in their groups, but
the groups matter only when the scheduler is on. Without the disk's
properties, as "What Seastar needs to know about the disk" explained,
the scheduler is switched off: every request goes to the disk
immediately, whatever its group's shares. So each run below was made
twice: without the disk's properties, which shows the disk without an
I/O scheduler, and with `iotune`'s, which shows what the scheduler
changes.

First, the program was run with its default options: one read in
flight, and 16 writes of 128 KB in flight. The writes slowed the reads
only a little. Alone, a read took 0.050 ms on average; beside the
writes, it took 0.072 ms, and 0.17 ms for the slowest 1%. The results
were the same with the disk's properties and without them, and with any
shares. The laptop's disk, an NVMe SSD, served the one read quickly even
among those writes, so the scheduler had nothing to improve.

So the program was run harder: 64 reads in flight, and 64 writes of a
megabyte, each group trying to use the whole disk. Without the disk's
properties, and then with them, each with equal shares, and with the
writes' shares lowered to 100 (runs of five seconds each, the best of
eight without the properties and of three with them):

| Properties | Shares (reads:writes) | Reads a second | Slowest 1% | Writes |
|---|---|--:|--:|--:|
| none | 1000:1000 | 176,457 | 0.69 ms | 1,077 MB/s |
| none | 1000:100 | 174,532 | 0.72 ms | 1,072 MB/s |
| `iotune`'s | 1000:1000 | 145,113 | 1.23 ms | 789 MB/s |
| `iotune`'s | 1000:100 | 262,632 | 0.99 ms | 170 MB/s |

Without the properties, the shares changed nothing: every request went
to the disk immediately, and the disk decided the order in which to
serve them. With the properties, the shares divided the disk's time
between the reads and the writes. To see this, multiply the number of
requests by what each costs. A read of 4 KB costs 3.4 µs, as the
previous section said, and a write of a megabyte (1,024 KB) costs
672 µs. With equal shares, the disk did 145,113 reads a second, which
took 0.49 seconds of its time, and 752 writes a second (the table's
789 MB/s, in millions of bytes), which took 0.51 seconds. So the reads
and the writes each got half of the disk's time. With the writes'
shares lowered to 100, the reads got 89% of the disk's time, and the
writes 11%. Shares of 1,000 and 100 ask for 91% and 9%, so the
scheduler came close.

With the properties, the disk also did less work in all, as the same
multiplication shows. With either shares, the reads and the writes
together took 1.00 second of the disk's time in each second: that's the
most that the scheduler sends. Without the properties, with equal
shares, the disk did 176,457 reads a second, which took 0.60 seconds by
`iotune`'s costs, and 1,077 MB of writes, which took 0.69 seconds: 1.29
seconds of work in each second. So when it does reads and writes
together, this disk can do 29% more than `iotune`'s numbers say.
`iotune` measures reads and writes separately, and the scheduler adds
the cost of a request's operation to the cost of its bytes, and the
costs of reads to those of writes, as if the disk did one thing at a
time. This disk does a mix of them faster than that, and the
scheduler, which believes the costs, kept back more than a fifth of
what the disk could do.

The slowest reads also waited longer with the scheduler: 1.23 ms with
equal shares, and 0.99 ms with the writes' shares lowered, against about
0.7 ms without it. They now waited in Seastar's queue, for tokens,
rather than in the disk's.

What the scheduler gives in return is control: here, the reads got the
disk when they needed it, whatever the writes did, in the proportion
that the shares asked for. A server whose reads are waited for by
clients, and whose writes are background work, wants exactly that.

> **Using the rest of the disk.** Seastar's developers have proposed
> ways to make the costs closer to what a disk really does. A pull
> request from 2025, <https://github.com/scylladb/seastar/pull/2801>,
> proposed making a request's cost the larger of its two parts, rather
> than their sum. Its author, from Redpanda, had found disks on which
> the scheduler allowed only about 60% of the operations a second that
> their properties listed. It was merged by mistake and unmerged, and it
> hasn't been submitted again. An issue from 2026,
> <https://github.com/scylladb/seastar/issues/3760>, and its draft pull
> request, <https://github.com/scylladb/seastar/pull/3766>, have the
> scheduler correct the disk's rates while the program runs, from the
> time its requests spend in the disk. But they only lower the rates,
> for a disk that is slower than its properties, and never raise them
> above the properties: they wouldn't let this disk do the 29% more
> that it can.

## Capping a group's bandwidth {index="I/O scheduler!bandwidth cap"}

Shares divide the disk only while it's busy: a group alone on the disk
may use all of it. Sometimes a group must not, even then: a background
job may need to leave some of the disk's bandwidth to another program
on the same machine, which Seastar's shares know nothing about. For
this,
[`scheduling_group::update_io_bandwidth()`]{index="`seastar::scheduling_group`!`update_io_bandwidth()`" .main}
caps the bytes a second that a group's requests may transfer, busy disk
or not. The cap isn't per shard: it limits what the group's requests
transfer on all the shards together. A supergroup, a group of groups
that [chapter](#ch:scheduling-groups) shows, can be capped the same way.
Redpanda caps some of its background work like this.

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
program starts a hundred reads together, with one intent, and cancels
them a millisecond later:

```{.cpp include="examples/io-scheduler/cancel.cc" snippet="cancel"}
```

Each of these reads, of a megabyte, costs 163 µs, so the millisecond, with the latency
goal's worth that the bucket held when the reads began, pays for about
ten of them. Without the disk's properties, the scheduler sent all of the
reads to the disk immediately, and there was nothing left to cancel; with
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
[chapter](#ch:observability) will show how to collect, for each disk,
shard and class:

- `io_queue_queue_length`: the requests waiting in Seastar's queue.
- `io_queue_disk_queue_length`: the requests sent to the disk and not
  yet completed.
- `io_queue_delay`: how long the latest request waited in the queue.
- `io_queue_total_read_ops` and `io_queue_total_write_ops`: the reads
  and the writes sent to the disk.
- `io_queue_total_read_bytes` and `io_queue_total_write_bytes`: the
  bytes of those reads and writes.
- `io_queue_total_split_ops`: the requests that were split because they
  were larger than the maximum length.
- `io_queue_starvation_time_sec`: how long a class had requests waiting
  while none of its requests were at the disk.

The scheduler also has LTTng tracepoints for every request, which
Seastar is built with by default, and which the section "Tracing" of
[chapter](#ch:observability) will show.

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
