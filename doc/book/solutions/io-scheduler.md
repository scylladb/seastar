# The I/O scheduler {#sol:io-scheduler}

## Exercise 1

With the disk's properties, 64 reads and 64 writes of a megabyte in
flight, and equal shares, the best of three runs for each goal:

| Latency goal | Reads a second | Slowest 1% | Writes |
|---|--:|--:|--:|
| 0.25 ms | 145,465 | 0.74 ms | 705 MB/s |
| 0.75 ms (the default) | 145,113 | 1.23 ms | 789 MB/s |
| 2 ms | 145,554 | 1.20 ms | 786 MB/s |
| 5 ms | 145,155 | 1.14 ms | 787 MB/s |

Only the lowest goal changed anything. The latency goal is the size of
the token bucket, and so also the largest request that the scheduler
sends whole: it splits a request whose cost wouldn't fit in a full
bucket. With `iotune`'s numbers, a write of a megabyte costs 672 µs,
which fits in a bucket of 0.75 ms or more. In a bucket of 0.25 ms, the
largest write that fits is 256 KB, at 187 µs (512 KB would cost 348
µs), so each write of a megabyte went to the disk as four. The metric
`io_queue_total_split_ops` confirms it: in an `io_tester` run of the
same workload, with a goal of 0.25 ms, all 545 writes of a megabyte were
split, into 2,180 requests, and with goals of 0.75 and 5 ms, none were.

That explains the writes. Four writes of 256 KB cost 4 × 187 = 748 µs of
the disk's time, rather than 672 µs for one of a megabyte, since each
pays for a request of its own. The writes still got half of the disk's
time, so they should have done 789 × 672 / 748 = 709 MB a second: they
did 705. The reads, which got the other half, did as many as before, and
the slowest of them waited much less, 0.74 ms rather than 1.23 ms,
which is as little as without the scheduler at all: a read that came
behind writes in the disk now waited for smaller ones.

A goal above the default changed nothing, since writes of a megabyte
were already sent whole, and the size of the bucket matters only when
tokens can pile up in it. Here they never did: both groups always had
requests waiting, so the scheduler spent the tokens as fast as they
came. A larger goal matters for a disk that has been idle, when the
full bucket lets a burst through at once, as the chapter's cancelling
example showed, and for requests larger than a megabyte, which it lets
through whole.

So here, the latency goal traded the writes' throughput for the reads'
latency, through the size of the requests that the disk got.

## Exercise 2

The best of three runs for each, with the disk's properties and the
reads' shares at 1000:

| Shares (reads:writes) | Reads a second | Slowest 1% | Writes |
|---|--:|--:|--:|
| 1000:300 | 222,046 | 0.94 ms | 382 MB/s |
| 1000:30 | 281,438 | 0.69 ms | 72 MB/s |

A read costs 3.42 µs of the disk's time, and a write of a megabyte
(1,048,576 bytes, while the program's MB is a million) costs 672 µs.
With shares of 300, the reads used 222,046 × 3.42 µs = 0.76 seconds of
each second, and the writes, 364 a second, 364 × 672 µs = 0.24 seconds:
76% and 24%, where the shares asked for 1000/1300 = 77% and 23%. With
shares of 30, the reads used 281,438 × 3.42 µs, 96%, and the writes, 69
a second, 4.6%, where the shares asked for 97% and 2.9%.

So the disk's time followed the shares to within a point or two. As
ratios, the second case looks worse, 21 to 1 rather than 33 to 1, but
when one side's share is that small, a point of the disk's time
changes the ratio a lot. Part of the reason is the reads themselves:
with 64 in flight, alone, they did 285,389 a second, 97.6% of the disk
as the scheduler counts it, so they could barely use the 97% that the
shares offered them, and what they didn't use went to the writes, as
fair queueing intends. The slowest reads also improved as the writes'
share shrank, 0.94 ms and 0.69 ms against 1.23 ms with equal shares,
since fewer writes were in the disk ahead of them.

## Exercise 3

The edited properties keep the reads' numbers, and divide both write
numbers by four:

```
disks:
  - mountpoint: /home
    read_iops: 358333
    read_bandwidth: 6547300352
    write_iops: 9906
    write_bandwidth: 405297696
```

With the program's defaults, one read at a time, and 16 writes of
128 KB in flight, the best of three runs:

```
alone:
  reads: 0.050 ms on average, 0.058 ms for the slowest 1%
with the writes:
  reads: 0.060 ms on average, 0.162 ms for the slowest 1%
  writes: 291 MB a second
```

With the true properties, the writes did about 950 MB a second. Now the
scheduler believes that a write of 128 KB costs 424 µs of the disk's
time (1/9,906 of a second, and 128 KB at 405 MB a second), so it sends
at most 2,358 of them a second, which is 309 MB. The reads, about
20,000 a second at 3.4 µs each, use 7% of the disk's time as the
scheduler counts it, which leaves 93% of 309 MB, 288 MB a second: the
writes did 291. The reads were a little faster beside the writes than
with the true properties, 0.060 ms on average rather than 0.072, since
the disk had fewer writes to do at once.

So properties that claim a slower disk are a way to keep background
work from using all of a disk, at the price of the throughput that it
gives up. ScyllaDB doesn't do it that way: its background work,
compaction, has a scheduling group of its own, whose shares limit it
only while something else wants the disk, as the chapter's table
showed, and let it use the whole disk otherwise. (ScyllaDB also adjusts
those shares as it runs, giving compaction more when it falls behind,
with the backlog controllers of
[chapter](#ch:scheduling-groups).)
