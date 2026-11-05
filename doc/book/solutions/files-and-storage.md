# Files and storage {#sol:files-and-storage}

## Exercise 1

`random-reads.cc` already takes the block's size, so the exercise is
three runs (the last with fewer reads, since each takes longer):

```
random-reads --block 65536
random-reads --block 1048576 --reads 5000
```

On the laptop (reads a second, and megabytes a second; the best of
three runs of each):

| Block | 1 in flight | 4 | 16 | 64 |
|---|--:|--:|--:|--:|
| 4 KB | 20,679 (85) | 79,317 (325) | 201,014 (823) | 275,096 (1,127) |
| 64 KB | 7,831 (513) | 31,574 (2,069) | 62,233 (4,078) | 72,841 (4,774) |
| 1 MB | 1,236 (1,296) | 6,148 (6,447) | 6,480 (6,795) | 6,350 (6,658) |

With 4 KB blocks, the reads a second grow with the reads in flight, to
more than 270,000, and the bytes a second stay small, about a gigabyte:
the disk is limited by the number of requests it can serve, and a deeper
queue lets it serve more of them at once. With 1 MB blocks, the bytes a
second stop near 6.8 GB/s, already with 16 in flight: the limit is now
the bytes, and more reads in flight only make each wait longer, 10 ms
on average with 64. Blocks of 64 KB are in between. A disk has both limits, and which one a program meets depends
on the size of its requests. That's why chapter 18's I/O scheduler,
which divides a disk among several kinds of work, can't count only
requests, or only bytes: it counts both.

## Exercise 2

The client is a few lines of Python, which send a pipeline of `SET`s on
one connection, all at once, and wait for all the replies. Its `SET`s
are 29 bytes or so each, so a commit of up to 141 of them fits in one
block. For 100,000 `SET`s, version 10 took 2.4 seconds, and its log
grew by 791 blocks: 791 commits, of about 126 requests each. That's the
connection's limit of requests in flight, 128. With `always`, every
`SET`'s reply waits for a commit, so after 128 `SET`s, `serve()` stops
reading, and the requests that the next commit could carry wait in the
socket, until the commit ends.

The solution is a patch to Shoal version 10,
`examples/solutions/shoal-files.patch`. A request on the connection's own
shard is executed at once, so if it logged a change, its reply waits
only for the commit:

```{.cpp include="examples/solutions/shoal-files.patch" file="main.cc" snippet="executed"}
```

Such a reply is added to the queue without the units of `reserve()`.
But then nothing would limit how many wait: a small request holds no
units of the request memory, and a reply counts against the reply memory
only when it's ready. So `add_executed()` takes units of the reply
memory at once, for the queue's entry, and for the request's bytes in
the log's tail, and gives them back when the reply is ready, which then
counts as every reply does:

```{.cpp include="examples/solutions/shoal-files.patch" file="main.cc" snippet="add-executed"}
```

`consume_units()` doesn't wait; the loop in `serve()` does, before it
reads the next request, while the reply memory is used up. With the
change, the 100,000 `SET`s took 0.065 seconds rather than 2.4, and a
million took 0.64 seconds rather than 24. The log was only a few
kilobytes bigger than its requests, the headers and padding of a few
commits: now every commit carries all the requests that arrived during
the one before it, and Shoal reads them as fast as the client sends.

The change helps a pipelining client with `always`. With many clients,
each with its own pipeline, the requests of all of them already shared
the commits, and the benchmark of the chapter didn't gain: with
`always`, the patched Shoal did 401,000 `SET`s a
second, and version 10 438,000. The patched Shoal is slower
because of exercise 3's checksums, which come next.

The solution's tests are in `test/test_solutions.py`; the first sends
the 100,000 `SET`s, and kills Shoal, and finds them all after the
restart:

```{.python include="examples/solutions/shoal-files.patch" file="test_solutions.py" snippet="pipeline-test"}
```

## Exercise 3

The solution is in the same copy of Shoal. Each commit begins with a
header, which looks like a request to the parser, `COMMIT` with two
arguments: the length of the commit's requests, and their CRC-32
checksum, each as eight hexadecimal digits, so that every header has the
same size, 44 bytes. A commit's header goes where its first request
would, and is filled in just before the commit is written, when its
requests are known:

```{.cpp include="examples/solutions/shoal-files.patch" file="log.cc" snippet="seal"}
```

`append()` calls `begin_append()` first, and `write_tail()` calls
`seal_commit()` first, and sets `_commit_start` to the end of the
commit's padding, where the next commit begins. When a write fails, its
commit's blocks stay in the tail, header and all, and the next commit
writes them again, before its own. Boost's CRC
(`boost::crc_32_type`, from `<boost/crc.hpp>`) does the arithmetic; any
32-bit checksum would do.

When Shoal starts, before it replays the log, it checks it, commit by
commit. Every commit begins at the start of a block, after the padding
of the one before it, so the check can find each from the one before,
and, after a commit that isn't valid, can try every later block for a
valid one:

```{.cpp include="examples/solutions/shoal-files.patch" file="log.cc" snippet="commit-header"}
```

A crash in the middle of a commit leaves a damaged commit at the end,
followed by nothing valid: the commits after it were never written,
since a commit is written only after the commits before it are. So the
check cuts it off, as version 10 did. But a valid commit after the
damage means that something damaged a commit that had been written
whole, such as a failing disk, and cutting there would throw away the
commits after the damage, whose clients were told that their changes
were safe. So Shoal refuses to start, and names the place, as Redis
does. Replaying skips the headers.

Each commit costs 44 more bytes, which usually fit in its padding. The
checksums cost more: with `everysec`, the benchmark of the chapter did
917,000 `SET`s a second rather than 1,011,000, and `perf` found 7% of the
time in Boost's CRC, which computes a byte at a time, from a table. A
CRC that the processor computes, such as CRC-32C, with the SSE 4.2
instruction `crc32` (which ScyllaDB's commit log uses), would cost a
fraction of that. The tests damage the last of a hundred commits, and one in
the middle, and check that Shoal cuts off the first, and refuses to
start with the second.

## Exercise 4

The failing file is in the same copy. A `layered_file_impl` doesn't
forward anything, so the class has a line for each of `file_impl`'s
operations; these are the ones that matter:

```{.cpp include="examples/solutions/shoal-files.patch" file="log.cc" snippet="failing-file"}
```

`open()` wraps the log's file in it, when the option `--fail-log-after`,
which only the tests use, says after how many writes.

With `always`, the clients whose changes were in the failed commit get
an error, `can't write the log`, which `execute_here()` makes of the
failed commit. (Writing this solution found that version 10 first left
them without any reply: a failed reply's future stopped the
connection's writer, while its reader went on waiting for the client's
next request, and the client waited for the reply forever. The error
reply is now in version 10 itself.) But the `SET` was executed, and its
new value is in memory, where other clients see it, although it may be
gone after a restart. And the next changes are executed too, each
getting the same error after its own failed commit. With `everysec`,
it's worse: the fiber that commits once a second logs an error, and the
clients go on getting `OK` for changes that will be lost, until the tail
grows beyond a megabyte, and the changes start waiting for commits that
fail.

Redis, when a write to its file fails, refuses changes with an error,
`MISCONF`, until a write succeeds (and with `always`, it exits). The
solution refuses them for good, as ScyllaDB stops serving: a failing
disk usually goes on failing. The store remembers why, and every shard
refuses, since a client's change may involve any shard's keys:

```{.cpp include="examples/solutions/shoal-files.patch" file="commands.hh" snippet="log-error"}
```

```{.cpp include="examples/solutions/shoal-files.patch" file="commands.cc" snippet="log-failed"}
```

`committed()` calls `log_failed()` when a commit fails, and so does the
fiber that commits once a second. Each command in the table now says
whether it may change the data (`.changes = true`), and `execute()` and
`execute_here()` refuse such a command, before executing it:

```{.cpp include="examples/solutions/shoal-files.patch" file="commands.cc" snippet="refuse-changes"}
```

A failure still costs the changes executed since the last good commit,
which the other clients may already have seen. But after that, no
client is told that a change was made when it wasn't. The tests start
Shoal with `--fail-log-after`, with `always` and with `everysec`, and
check that changes are refused, and that reads still work.

## Exercise 5

The client sends `SET k0 0`, `SET k1 1`, and so on, in batches of a
thousand, and counts the replies; after three and a half seconds, the
script kills the server, starts it again, and counts the acknowledged
keys that are missing:

```{.python include="examples/solutions/files-and-storage-ex5.py"}
```

On the laptop, the first line with Shoal, and the second with Valkey
(three runs each, the others alike):

```
3400000 SETs acknowledged (971271 a second); 13000 missing after the restart, 13.4 ms worth
3223000 SETs acknowledged (920611 a second); 0 missing after the restart, 0.0 ms worth
```

Shoal lost between 314 and 13,000 acknowledged `SET`s in the three
runs, up to 13 ms worth. That's not a second's worth: with the client
sending as fast as it can, the tail reaches a megabyte, about 30,000 of
these `SET`s, many times a second, and each time, the changes wait for
a commit. A slower client,
which never fills a megabyte, would lose up to a second. Valkey lost
nothing: before it replies, it writes its file with `write()`, and a
`write()` to a file is in the kernel's memory as soon as the call
returns, where a crash of Valkey can't touch it. Only a crash of the
whole computer, or of its power, loses the last second, which is what
`everysec` promises. For Shoal, that would lose the same as killing it,
plus up to a second of commits whose writes the disk hadn't stored yet.

To match Valkey, Shoal would have to get the tail out of its own memory
before replying, and with direct I/O, the only place to put it is the
disk, which is `always`, without the flush: an option that Shoal
doesn't have. The disk keeps what it was given, unless the power fails.
