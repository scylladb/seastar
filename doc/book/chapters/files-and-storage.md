# Files and storage {#ch:files-and-storage}

So far Shoal has kept everything in memory, and lost it all when it
stopped. Redis is in memory too, but it can also keep its data on disk,
so that it survives a restart, or a crash: in its *append-only file*, it
records every command that changed the data, and when it starts, it
executes them all again. This chapter explains how a Seastar program
works with files: why it reads and writes them in a way that most
programs don't, how to read them at random places, how to make sure that
what it wrote is really on the disk, and how to read and write them as
streams. Then Shoal version 10 puts it all together, and keeps an
append-only log of its own, on every shard.[^shoal-v10]

[^shoal-v10]: The source of this version is in `shoal/v10/`.

## Direct I/O {index="direct I/O"}

An ordinary program reads a file with `read()`, and Linux answers from
its *page cache*, the copies of files' contents that it keeps in memory,
or, if the data isn't there, reads it from the disk first. The program
doesn't notice the difference, except that the call takes longer. A
Seastar shard would notice: as [chapter](#ch:how-seastar-runs-your-code)
explained, a system call that waits for the disk stops the whole thread,
and with it every task of the shard.

So Seastar never reads or writes a file through the page cache. It
opens every file for [*direct I/O*]{.index .main}, with the flag
`O_DIRECT`, and sends its requests to the kernel asynchronously, through
`io_uring` or Linux AIO: the shard goes on with other work, and collects
the results when they come. (With Linux AIO, only direct I/O is truly
asynchronous: an ordinary read may block inside the call that was
supposed to start it in the background.)

Direct I/O makes the disk transfer the data straight to and from the
program's memory, by
[DMA]{index="DMA (direct memory access)" .main}[]{index="direct memory access|see{DMA (direct memory access)}"}:
the disk's controller writes the data into the
program's own buffer, or reads it from there, with no copy in between.
This is why Seastar's file functions have "dma" in their names, from
`open_file_dma()` to `dma_read()` and `dma_write()`, and why their
buffers must follow the rules described below.
(Ordinary I/O uses DMA too, but into the kernel's page cache, from which
`read()` then copies the data with the CPU.)

Direct I/O has a second benefit: the program decides what to keep in
memory, which it knows better than the kernel does. ScyllaDB keeps its
own cache of rows, and never lets the kernel cache its files.

Direct I/O comes with rules. The disk transfers whole blocks, so the
position and the size of every read and write must be multiples of the
disk's block size, and the address of the memory they use must be
aligned too. A
Seastar `file` says what those multiples are, with
[`disk_read_dma_alignment()`]{index="`seastar::file`!`disk_read_dma_alignment()`" .main},
[`disk_write_dma_alignment()`]{index="`seastar::file`!`disk_write_dma_alignment()`" .main}
and
[`memory_dma_alignment()`]{index="`seastar::file`!`memory_dma_alignment()`" .main}:
4,096 bytes for all three, on the laptop of this book's measurements,
and 512 on some disks. Seastar works them out when it opens the file, by
asking the file system and the device, and they needn't be the same: a
write's may be larger than a read's, and the disk's properties
([chapter](#ch:io-scheduler)) can make it larger still, while a kernel
since Linux 6.1 may report that the memory needs less alignment than
the disk's blocks. Reads have helpers that hide the rules, as the
next section shows. Writes don't, and Shoal's log, later in this
chapter, is shaped by them. (For tests on a machine whose file system
doesn't support direct I/O, Seastar's option
[`--kernel-page-cache`]{index="command-line options!`--kernel-page-cache`"}
goes through the page cache instead.)

## Reading at random {index="random reads"}

A file is opened with
[`open_file_dma()`]{index="`seastar::open_file_dma()`" .main}, from
`<seastar/core/seastar.hh>`, which returns a future of a
[`file`]{index="`seastar::file`" .main}, from `<seastar/core/file.hh>`:

```{.cpp include="examples/files-and-storage/random-reads.cc" snippet="open"}
```

The flags are those of the system call `open()`, as an
[`open_flags`]{index="`seastar::open_flags`" .main}, from
`<seastar/core/file-types.hh>`: `ro`, `wo` or
`rw`, and `create`, `exclusive` and `truncate`. A `file` is a handle:
copies of it refer to the same open file, which stays open until
`close()`, which, like a stream's, a program calls, and waits for, when
it's done with the file. A `file` also belongs to the shard
that opened it. To use the same file on another shard,
[chapter](#ch:shared-nothing) mentioned `file::dup()`, which makes a
`file_handle` that the other shard turns into a `file` of its own with
`to_file()`.

This program, `random-reads.cc`, writes a file of a gigabyte of random
bytes, and then reads 4 KB blocks of it, at random places, first one at a time, then
with more and more reads in flight at once:

```{.cpp include="examples/files-and-storage/random-reads.cc" snippet="read"}
```

[`dma_read<char>(pos, len)`]{index="`seastar::file`!`dma_read()`" .main}
returns a future of a `temporary_buffer<char>` with the data. It's one of
the helpers that hide direct I/O's rules: it accepts any position and
length, reads the aligned blocks around them, and returns only the bytes
asked for, as a part of the aligned buffer, which costs no copy. It may
return fewer bytes, at the end of the file;
[`dma_read_exactly()`]{index="`seastar::file`!`dma_read_exactly()`" .main}
fails instead. `max_concurrent_for_each()`, which
[chapter](#ch:concurrency-within-a-shard) used to connect a few clients
at a time, keeps `in_flight` reads going at once. Its output, on the
laptop's NVMe SSD (under the disk encryption that Fedora sets up, and
the btrfs file system):

```
  1 in flight:   20679 reads a second, each 0.048 ms on average, 0.055 ms for the slowest 1%
  4 in flight:   79317 reads a second, each 0.050 ms on average, 0.070 ms for the slowest 1%
 16 in flight:  201014 reads a second, each 0.079 ms on average, 0.124 ms for the slowest 1%
 64 in flight:  275096 reads a second, each 0.227 ms on average, 0.335 ms for the slowest 1%
```

One read at a time, the disk answers in about 0.05 ms, which isn't much
for one read, but limits the program to about 20,000 reads a second.
With more reads in flight, the disk works on several at once: four are
almost four times as many reads a second, with hardly any more waiting,
and sixteen almost ten times. But 64 give only 37% more than sixteen,
and each read waits almost five times as long as alone. That's the trade-off
of every disk: more requests at once buy throughput with latency, and
beyond some point, nothing but latency. Deciding how many requests to
send at once, so that the disk is busy but no request waits too long,
is the job of Seastar's I/O scheduler, the subject of
[chapter](#ch:io-scheduler).

Why random bytes? The first version of this program filled its file
with the letter `x`, and found single reads three times slower, and
large reads faster than the disk can deliver them. Fedora mounts its
btrfs file system with compression, and btrfs reads compressed data
through its own cache, and decompresses it, direct I/O or not: the
program was measuring the file system, not the disk. Random bytes don't
compress. A file system that compresses, or that changes what it
writes in other ways, isn't for a Seastar program's data, whose I/O
Seastar wants to schedule itself: ScyllaDB requires XFS. (Shoal's log,
which is text, compresses well, so this chapter's measurements of Shoal
put its directory where btrfs doesn't compress, with `chattr +m`.)

## Writing, and making it last {index="durability"}

Writes have no helpers that hide the rules.
[`dma_write(pos, buffer, len)`]{index="`seastar::file`!`dma_write()`" .main}
writes `len` bytes from `buffer` at position `pos`, and all three must
be aligned. The buffer comes from
[`temporary_buffer<char>::aligned()`]{index="`seastar::temporary_buffer`!`aligned()`" .main},
which allocates memory at the alignment given, as `random-reads.cc`'s
`fill()` does:

```{.cpp include="examples/files-and-storage/random-reads.cc" snippet="write"}
```

When `dma_write()`'s future resolves, the data is in the disk's hands,
but not necessarily on its storage: the disk may keep it in a cache of
its own, and lose it if the power goes out. Only
[`flush()`]{index="`seastar::file`!`flush()`" .main} promises that
everything written to the file so far is *durable*, that it will survive
a crash: it calls `fdatasync()`, which waits until the disk confirms
that the data is stored. That takes time, about 3 ms on the laptop,
for a write of 4 KB, and not much more for a megabyte, and it's a cost that every database designs around, as Shoal's
log will. (For tests that don't care about crashes, Seastar's
[`--unsafe-bypass-fsync`]{index="command-line options!`--unsafe-bypass-fsync`"}
makes `flush()` return at once.)

There is one more thing to make durable: the file's name. A file's
contents and its entry in a directory are stored separately, and
`fdatasync()` on the file makes only the contents durable. A file
created just before a crash may be gone after it, contents and all,
however carefully they were flushed. So after creating, renaming or
deleting a file, a program that cares calls
[`sync_directory()`]{index="`seastar::sync_directory()`" .main}, from
`<seastar/core/seastar.hh>`, on its directory.

Together, these make the classic way to replace a file so that a crash
leaves either the old file or the new one, never half of each: write the
new contents under a temporary name, flush them, rename the file over
the old one with
[`rename_file()`]{index="`seastar::rename_file()`" .main}, which Linux
does atomically, and flush the directory. Shoal version 10 does it once,
for a small file that records how many shards wrote its logs, since a
key's shard depends on their number:

```{.cpp include="shoal/v10/log.cc" snippet="shard-count"}
```

[`file_exists()`]{index="`seastar::file_exists()`" .main} and
`rename_file()` come from `<seastar/core/seastar.hh>`, with the other
functions on files and directories: `remove_file()`, `link_file()`,
`file_size()`, `file_stat()`, `make_directory()`,
`recursive_touch_directory()`, which makes a directory and any parents
it lacks, and `open_directory()`, whose `file` lists the directory's
entries with `list_directory()`.
[`util::read_entire_file_contiguous()`]{index="`seastar::util::read_entire_file_contiguous()`" .main},
from `<seastar/util/file.hh>`, reads a small file into an `sstring`.

## The system-call thread {index="system-call thread"}

Reads, writes and `flush()` reach the disk through the reactor backend,
which, as [chapter](#ch:how-seastar-runs-your-code) described, lets the
shard hand a request to the kernel, and go on running. Most of the other
file operations have no such form, at least none that Seastar uses:
opening a file, `file_stat()` and `file_size()`, which make the `stat()`
system call, renaming, removing and linking files, making directories
and listing their entries, a file's `truncate()` and `allocate()`, and
closing a file that was open for writing. (Linux's io_uring can open,
rename and remove files asynchronously, but Seastar doesn't use it for
these.) Any of these system calls may wait: to read a directory or a
file's metadata from the disk, to free the blocks of a large file, or
for the server of a network file system. Starting another program,
with `spawn_process()` ([chapter](#ch:threads)), is the same.

So Seastar doesn't make these calls in the shard's thread. Each shard
has a helper thread, its
[*system-call thread*]{index="system-call thread" .main}, named
`syscall-0`, `syscall-1` and so on, which makes them for it. A call
becomes a request in a queue, which the shard shares with the thread,
and a future. The thread, which sleeps until the shard wakes it, makes
the requests' system calls one after the other, and puts each result in
a second queue. The shard picks the results up when it polls, as it
picks up finished reads, and resolves the futures, and if the shard is
asleep, the thread wakes it. That's why `open_file_dma()` and
`remove_file()` return futures, although each is a single system call.
The queue holds up to 128 requests, and more wait for room.

`syscall-thread.cc` measures what this costs. First, a call that the
thread makes quickly:

```{.cpp include="examples/files-and-storage/syscall-thread.cc" snippet="cost"}
```

Then a slow one. The program writes a file of 200,000 blocks of 4 KB,
with a hole after each, which the file system stores as 200,000 separate
pieces, removes it, and meanwhile asks for another file's size, on the
same shard and on another one:

```{.cpp include="examples/files-and-storage/syscall-thread.cc" snippet="slow"}
```

Its output, on two shards:

```
file_size(): 3.3 us, stat() in the shard: 0.3 us
remove_file(): 195 ms
file_size() on this shard, started with it: 195 ms
slowest file_size() on shard 1, meanwhile: 0.39 ms
sleeps of 1 ms on this shard, meanwhile: 100
```

Each line shows something:

- A call through the system-call thread costs about 3 microseconds,
  more than ten times as much as the `stat()` system call that it makes:
  the rest is the trip between the two threads. That's nothing next to
  opening a file, but it's a reason not to make calls that a program can
  avoid, such as opening the same file again and again. Shoal's log,
  later in this chapter, opens its files once, and keeps them open.
- The system-call thread makes one call at a time. Removing the file of
  many pieces took 195 ms, and the `file_size()` sent right after it
  took as long, although `stat()` takes a fraction of a microsecond: it
  waited for its turn. One slow file operation delays all of its shard's
  later file operations, opening files included.
- Another shard has a system-call thread of its own, and its
  `file_size()` didn't wait.
- The shard itself went on running, but more slowly. In those 195 ms,
  it finished only 100 sleeps of 1 ms, although on an idle shard, a sleep
  of 1 ms typically ends only 0.02 ms late. A system-call thread is pinned to its
  shard's CPU core, as the `Cpus_allowed_list` line of the thread's
  `/proc/PID/task/TID/status` shows, so the kernel's work for the call,
  freeing the file's pieces, took half of the shard's CPU time.

Seastar has functions for most file operations: those of the previous
section, and others, such as `file_system_space()`, `chmod()` and
`file_accessible()`. ScyllaDB and Redpanda make the blocking system
calls themselves only in a few places, where they have a reason, or
don't need one:

- At startup, when a short wait harms no one, for calls that Seastar has
  no function for: ScyllaDB locks each of its directories with
  `lockf()`, so that a second ScyllaDB can't use them, and Redpanda
  writes its PID file with `stat()`, `unlink()` and `open()`.
- Where Seastar can't be used at all: Redpanda's crash recorder opens
  its file with `open()`, so that it can write the description of a
  crash, and `fsync()` it, from a signal handler, where only the
  simplest system calls are safe.
- And to learn a disk's size: both call `std::filesystem::space()`,
  which makes the `statvfs()` system call in the shard's thread, once
  in a while, although `file_system_space()` would make it in the
  system-call thread. On a local disk, `statvfs()` is quick.

The system-call thread is Seastar's own: no function gives it a call of
a program's own, and its one-at-a-time queue would be the wrong place
for long work anyway. A program that needs a library that blocks, such
as Redpanda's Kerberos authentication, gives it a thread of its own, as
[chapter](#ch:threads) shows. Seastar counts the system-call thread's
requests in the metric `reactor_io_threaded_fallbacks`, with a label,
`reason`, that tells file operations from process operations. (The
`linux-aio` backend has a second such thread, `aio-syscall-0` and so on,
for reads and writes that it can't submit without waiting, which the
same metric counts with the reason `aio_fallback`.)

## File streams {index="file streams"}

The file of the shard count, above, is written with an output stream, from
[`make_file_output_stream()`]{index="`seastar::make_file_output_stream()`" .main},
from `<seastar/core/fstream.hh>`: the same `output_stream<char>` as a
socket's, which collects the writes in aligned buffers of its own, and
writes them to the file with `dma_write()`. Its counterpart,
[`make_file_input_stream()`]{index="`seastar::make_file_input_stream()`" .main},
returns an `input_stream<char>` that reads a file from beginning to end,
and with a
[`file_input_stream_options`]{index="`seastar::file_input_stream_options`" .main},
reads ahead: `buffer_size` sets the size of each read, and `read_ahead`
how many more reads to keep in flight beyond the one the program waits
for, so that the disk is never idle while the program works on the data
it has. Shoal reads its log back this way, below, with the same parser
that reads its clients' requests from their sockets.

A file output stream has one limitation that matters for a log. It can
write only aligned blocks, so `flush()` pads the last, partial block
with zeros, writes it, and cuts the file back to its true size; then
the stream's next write would have to begin in the middle of a block,
which `dma_write()` can't do. So Seastar's documentation warns that
"`flush()` should be the last thing to be called on a file output
stream". That's fine for writing a file in one go, but a log must make
its data durable again and again, as it grows. Shoal's log does its own
buffering, below.

## Shoal, version 10 {index="Shoal!version 10"}

Redis's append-only file is a list of the commands that changed the
data, in the same protocol, RESP, in which clients send them. When Redis
starts, it reads the file, and executes every command again, in order,
which rebuilds the data. When does a command reach the disk? Redis's
option `appendfsync` decides. With `always`, Redis flushes the file
before it replies to a command that changed something, so an
acknowledged change is never lost. With `everysec`, the default, it
flushes once a second, and a crash of the computer loses up to a second
of acknowledged changes. With `no`, it leaves the decision to the
kernel. (Valkey has the same file, and the same options.)

Shoal version 10 keeps such a log, one for each shard, in the directory
that its option `--dir` names, with Redis's `--appendfsync`, `always` or
`everysec`. Each shard records its own changes there, and when Shoal
starts, each shard replays its own log, in parallel. The store holds
the shard's log:

```{.cpp include="shoal/v10/commands.hh" snippet="store-log"}
```

And this is the log:

```{.cpp include="shoal/v10/log.hh" snippet="log"}
```

### Recording the changes

The command that changed the data isn't always the request to record.
Some requests are recorded as they are: `LPUSH` and `RPUSH`, `PERSIST`,
an `LPOP` or `RPOP` that popped something. Others are recorded as what they did:
each key that a `DEL` deleted, as a `DEL` of its own, and each key that
an `MSET` set, as a `SET`, so that every shard records only its own keys.
And expiration times are recorded as times on the computer's clock, in
milliseconds since 1970, as Redis records them, since a duration such as
`EXPIRE`'s would start again when the log is replayed. So an `EXPIRE` is
recorded as a `PEXPIREAT`, and a `SET` with `EX` or `PX` as a `SET` with
`PXAT`: Redis's command `PEXPIREAT` and `SET`'s option `PXAT`, which
version 10 adds, for its log and for its clients. (A `SET` and a separate `PEXPIREAT` wouldn't do: a crash
between them would leave the key without its expiration time.) A
`BLPOP` that gets an element records an `LPOP`. Each of these commands
calls `record()`:

```{.cpp include="shoal/v10/commands.cc" snippet="record"}
```

`append()` turns the words into RESP, and copies them into the log's
*tail*: the requests that aren't in the file yet, which wait in chunks of
128 KB, allocated with the alignment that `dma_write()` needs:

```{.cpp include="shoal/v10/log.cc" snippet="append"}
```

Appending only copies bytes, without waiting for anything, so a command
that changes the data still runs to its end at once. Only its reply may
wait.

### Group commit {index="group commit"}

A *commit* writes the tail to the file, and flushes it. A flush takes
milliseconds, and with `--appendfsync always`, every change must wait for
one, so a commit made for each change would limit Shoal to a few hundred
changes a second. The answer, which every database uses, is
[*group commit*]{.index .main}: requests that wait for the disk at the same
time share one write and one flush.

```{.cpp include="shoal/v10/log.cc" snippet="commit"}
```

The semaphore of one unit lets one commit write at a time. A commit
remembers how much it must write, `end`, before it does anything else;
when it gets the semaphore, a commit that came before it may have written
that much already, and then it has nothing left to do. Before it even
asks for the semaphore, it yields, as `shard_queue` did in
[chapter](#ch:designing-a-sharded-application), so that the requests
that are being executed in the same turn of the reactor's loop, the rest
of a pipeline, or of a batch from another shard, append theirs, and share
its write.

`write_tail()` pads the last block with zeros, to the disk's alignment,
writes the chunks, and flushes. The requests appended later begin after
the padding, in the next block. So every block of the file is written
only once, and a crash in the middle of a write can damage only the
blocks of that write, never the requests of an earlier commit, which a
client may already have been told are safe. The price is up to a block
of zeros per commit. ScyllaDB's commit log makes the same choice, and
counts the bytes it wastes so, in a metric it calls "slack". When the
file is read back, the parser skips the zeros.

With `--appendfsync everysec`, the changes don't wait: a fiber in the
background commits once a second, until Shoal stops. A command waits for
a commit only if the tail has grown beyond a megabyte, so that a client
that changes data faster than the disk can take it is slowed down to the
disk's pace, rather than filling the memory. Both cases are in one
function, `must_commit()`, which `committed()` consults after a command
executes, to decide whether its reply waits for a commit:

```{.cpp include="shoal/v10/commands.cc" snippet="store-open"}
```

```{.cpp include="shoal/v10/commands.cc" snippet="committed"}
```

`execute_here()` now returns a future: usually a ready one, but with
`always`, a changing command's reply is ready only after the commit.
Every caller of `execute_here()` copes. The batch from another shard
executes all its requests, and then waits for their replies with
`when_all_succeed()`, so that its changes share a commit. And the fast
path of the connection's own shard writes a reply at once if it's ready,
and otherwise puts it in the queue of replies, which writes it when it's
ready, and the requests behind it go on being read and executed
meanwhile.

### Replaying the log {index="recovery"}

When Shoal starts, it opens the logs, after making sure that the
directory exists and that it was written by as many shards as Shoal has
now:

```{.cpp include="shoal/v10/main.cc" snippet="open-logs"}
```

Each shard opens its log, and reads it back through a file input stream,
with the same `request_reader` that parses the requests of a client:

```{.cpp include="shoal/v10/log.cc" snippet="open"}
```

Each request goes to `replay()`, which executes it, as if a client had
sent it, except `FLUSHALL`: a client's `FLUSHALL` runs on every shard,
and has no function for a single shard, so `replay()` clears the
shard's data itself:

```{.cpp include="shoal/v10/commands.cc" snippet="replay"}
```

`replay()` doesn't wait for the future that `execute_here()` returns. It
assumes that the future is already ready, since nothing waits for a
commit to a log that the store doesn't have yet, and drops it with
`ignore_ready_future()`. That assumption is a bug, which a `release`
build hides: [chapter](#ch:testing) finds it, with the `debug` build,
and version 16 fixes it.

The store gets its log only after the replay is over, so the requests
being replayed record nothing: `record()` checks for the log. The parser
learned two things for this, in version 10. It skips zeros between
requests, the padding of the log's commits. And it no longer loses the
requests that come before a protocol error in the same buffer: it keeps
the error until they have been read, and reports it only then, which
matters to a client too, whose requests before the error are now
executed, as they were before version 9's reader, which parses a whole
buffer at a time ([chapter](#ch:networking)).

A crash in the middle of a commit leaves the file's end damaged: a
request cut short, or a block of garbage. The replay stops at the first
request it can't parse, and Shoal cuts the file after the last whole one,
and logs how many bytes it cut. Redis does the same for a request cut
short at the end of its file, but refuses to start if its file is
damaged anywhere else, and offers a tool to repair it; Shoal can't tell
the two apart, and cuts whatever follows, which, after damage in the
middle of the file, would lose the good requests after it too. Exercise
3 gives the log what it needs to tell them apart.

### Stopping, and crashing {index="clean shutdown"}

[Chapter](#ch:clean-shutdown) promised that a server that writes to disk
needs a clean shutdown, and here is why. With `everysec`, acknowledged
changes may wait in the tail for up to a second. `store::stop()`, which
`sharded<store>::stop()` calls on every shard as Shoal stops, ends the
periodic commits, and closes the log, which commits what's left. A
`kill -9`, or a crash, gets no such chance, and loses the changes made
since the last commit. Redis with `everysec` loses less to a `kill -9`:
before it replies, it writes its log with `write()`, which leaves the
data in the kernel's memory, safe from a crash of Redis itself, and
only a crash of the whole computer loses the last second. Shoal's
uncommitted tail is in Shoal's own memory, and a write that would put it
in the kernel's would be a trip to the disk, since the log bypasses the
kernel's cache. Exercise 5 measures what each loses.

With `always`, nothing acknowledged is lost: version 10's tests kill Shoal with
`SIGKILL` after a hundred `SET`s, and find all of them after the restart.

### What it costs

The benchmark of the earlier chapters, on one shard, measured `SET`s of
random keys, in pipelines of 64 requests, on 64 connections, with Shoal
and Valkey each keeping a log, in a directory that btrfs doesn't compress.
The last two rows have no pipelines: each client waits for each reply
before it sends the next request.

| `SET`s a second | Shoal | Valkey |
|---|--:|--:|
| no log (Shoal version 9) | 1,508,000 | |
| `everysec` | 1,011,000 | 1,037,000 |
| `always` | 438,000 | 459,000 |
| `always`, 1 client, no pipeline | 337 | 483 |
| `always`, 50 clients, no pipeline | 8,200 | 15,100 |

With pipelines, the log costs Shoal a third of its speed with
`everysec`, and more than two thirds with `always`, and Shoal ends up a little
slower than Valkey with either. Group commit is what keeps `always`
usable: one client without a pipeline gets 337 `SET`s a second, a
flush's worth, about 3 ms each; with pipelines, more than a thousand
requests share each flush. With `everysec`, the flushes are a small
part of the cost: with `--unsafe-bypass-fsync`, Shoal did 1,110,000
`SET`s a second, only a tenth more. The rest of the cost is in recording the requests, spread thin
in a profile; the largest part, about 7% of the time, is formatting
the log's lines, since `fmt::format_to()` reads its format string as it
runs.

One client without a pipeline is slower with Shoal than with Valkey:
3.0 ms for each request, rather than 2.1. Part of the difference is that
Shoal's commit makes two trips to the disk, `dma_write()` and then the
flush, while Valkey's `write()` only copies the data into the kernel's
memory, and its `fdatasync()` makes one trip. Opening the log with
`open_flags::dsync`, which makes every write durable by itself, makes
Shoal's commit one trip too, and the one client got 372 `SET`s a
second, still fewer than Valkey's 483; the rest of the difference is in
how the file system handles each kind of write, which this chapter
leaves alone. And `dsync` lost much more than it gained: a commit with
many chunks paid for a trip per chunk, and with pipelines, Shoal did
591,000 `SET`s a second with `everysec`, and 359,000 with `always`. So
version 10 doesn't use it.

The 50 clients without pipelines get half of Valkey's speed for another
reason. Valkey's `fdatasync()` blocks its whole event loop, so while it
waits, all 50 clients' next requests arrive, and the next flush carries
all of them. Shoal doesn't block: while a commit writes, the clients
whose replies are already out send their next requests, which wait for
the next commit, and the clients fall into two groups whose commits take
turns. Each request waits for the end of the commit being written, and
then for its own. The log shows it: 500,000 `SET`s of 45 bytes took
20,000 blocks, one for each commit, 25 requests each, half of the 50
clients. A commit that could start before the previous one ends, with
each reply still waiting for every earlier commit, would let each
request wait for one commit only; it's a change this chapter leaves
out.

The log is bigger than the data: 20 million `SET`s made 900 MB of log,
45 bytes each, for 10 million keys, since every `SET` of a key that was
already set is recorded again. Valkey's file was about 430 MB at the end,
since Valkey *rewrites* its file, in the background, when it has
doubled: it writes the data as it is now, compactly, and switches to the
new file. Shoal's log, for now, grows forever. A rewrite must go on in
the background, while the commits go on in the foreground, and must not
slow them down; Shoal version 11 adds one, in
[chapter](#ch:scheduling-groups), with the tool that Seastar has for
exactly that, scheduling groups.

Replaying is fast: those 900 MB took 11.3 seconds, 1.8 million
requests a second, on one shard.

## Your own file {index="`seastar::file_impl`"}

Like a stream, a `file` is a handle to an implementation, a class that
derives from [`file_impl`]{index="`seastar::file_impl`" .main}, from
`<seastar/core/file.hh>`, and a program can put its own there: a file
that compresses, that encrypts, that counts, or that checks. Seastar's
[`layered_file_impl`]{index="`seastar::layered_file_impl`" .main}, from
`<seastar/core/layered_file.hh>`, is a convenient base, which holds the
file it wraps, and copies its alignments; it forwards nothing, so the
wrapper implements every operation of `file_impl`, most of them as a
line that calls the same operation on the wrapped file. To
use one, make a `file` from it, with `file(make_shared<my_file>(...))`,
and pass that `file` to anything that takes one, a file stream for
example. ScyllaDB wraps the files of its data and of its commit log in
a `checked_file_impl`,
whose every operation passes its errors to a handler: an I/O error on a
data file, which usually means a failing disk, makes ScyllaDB stop
serving, rather than go on with data it can't trust. Exercise 4 does the
same for Shoal.

## Exercises

1. Run `random-reads.cc` with blocks of 64 KB and of 1 MB, instead of
   4 KB (its option `--block`). How do the reads a second change, and
   the bytes a second? What does that say about what limits a disk: the
   number of requests, or the bytes?

2. With `--appendfsync always`, send version 10 a pipeline of 100,000
   `SET`s on one connection, and count its commits (the log grows by at
   least a block for each). How many requests share a commit, and what
   limits that number? Change Shoal so that a request whose reply waits
   only for the log doesn't count against the connection's limit of
   requests in flight, but its reply's memory still counts against
   `--max-reply-memory`, and measure again.

3. Make the log tell a damaged end, which a crash leaves, from damage in
   the middle, which a failing disk does: give every commit a small
   record of its own that holds a checksum of the commit's bytes, and,
   when replaying, treat a commit without a valid checksum as the end of
   the log only if nothing valid follows it.

4. Wrap the log's file in a `layered_file_impl` of your own whose
   `write_dma()` fails, as a failing disk would, after the first hundred
   writes. What do Shoal's clients see, with `everysec` and with
   `always`? Then make Shoal stop accepting changes after its log fails,
   with an error reply to each, as ScyllaDB stops serving after a disk
   error.

5. With `--appendfsync everysec`, write a client that sends `SET`s as fast
   as it can, counting those acknowledged, and kill Shoal with `SIGKILL`
   in the middle. After a restart, how many of the acknowledged `SET`s
   are missing, and how much time's worth is that? Do the same to Valkey,
   started with `--appendonly yes`. What would Shoal lose if, instead of
   Shoal, the computer crashed?
