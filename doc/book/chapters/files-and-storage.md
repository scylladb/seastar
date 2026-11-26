# Files and storage {#ch:files-and-storage}

So far Shoal has kept all its data only in memory, so the data was lost
when Shoal stopped. Redis keeps its data in memory too, but it can also
keep it on disk, so that the data survives a restart or a crash. Its
*append-only file* records every command that changed the data, and
when Redis starts, it executes them all again.

This chapter explains how a Seastar program works with files:

- why it bypasses the kernel's page cache, with direct I/O;
- how to open a file, and read from any position in it;
- how to write to it, and make sure that what was written is really on
  the disk;
- which file systems Seastar works well with;
- which file operations have no asynchronous form, and how Seastar runs
  them without stopping the shard;
- how to read and write a file as a stream;
- and how to wrap a file in a class of one's own, for example to encrypt
  it.

Shoal version 10 then uses these to keep an append-only log of its own
on every shard, and replays the logs when it starts.[^shoal-v10]

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
chapter, is shaped by them.

## Random-access reads {index="random access"}

A file is opened with
[`open_file_dma()`]{index="`seastar::open_file_dma()`" .main}, from
`<seastar/core/seastar.hh>`, which returns a future of a
[`file`]{index="`seastar::file`" .main} object, from `<seastar/core/file.hh>`:

```{.cpp include="examples/files-and-storage/random-reads.cc" snippet="open"}
```

The flags are those of the system call `open()`, as an
[`open_flags`]{index="`seastar::open_flags`" .main}, from
`<seastar/core/file-types.hh>`: `ro`, `wo` or
`rw`, and `create`, `exclusive` and `truncate`. A `file` object is a
handle: copies of it refer to the same open file, which stays open
until the program calls `close()`, and waits for it, as with a stream.
A `file` object also belongs to the shard that opened it. To use the
same file on another shard, [chapter](#ch:shared-nothing) mentioned
`file::dup()`, which makes a `file_handle` that the other shard turns
into a `file` object of its own with `to_file()`.

This program, `random-reads.cc`, writes a file of a gigabyte of random
bytes, and then reads 4 KB blocks of it, at random offsets, first one at a time, then
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
fails instead.

`max_concurrent_for_each()`, which
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

One read at a time, this disk answers in about 0.05 ms, which isn't much
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

Besides `dma_read<char>(pos, len)` and `dma_read_exactly()`, a `file` object
has three more read methods:

- [`dma_read_bulk<char>(pos, len)`]{index="`seastar::file`!`dma_read_bulk()`" .main}
  is `dma_read<char>(pos, len)` without the last step: it doesn't trim the buffer to
  `len` bytes, but returns everything from `pos` to the end of the
  aligned blocks that it read, for a reader that can use the bytes
  after the ones it asked for. Seastar's file input stream, later in
  this chapter, reads with it.
- `dma_read(pos, buffer, len)` reads into the program's own buffer.
- `dma_read(pos, iov)` reads into several buffers, given as a
  `std::vector<iovec>`. An `iovec`, from `<sys/uio.h>`, is the POSIX
  structure that the system calls `readv()` and `writev()` take: a
  buffer's address, `iov_base`, and its length, `iov_len`.

The last two don't hide direct I/O's rules: like the writes of the next
section, they need the position, the lengths and the buffers' addresses
all aligned, and they return the number of bytes read, which may be
fewer at the end of the file.

## Writing, and making it last {index="durability"}

Writes have no helpers that hide the rules.
[`dma_write(pos, buffer, len)`]{index="`seastar::file`!`dma_write()`" .main}
writes `len` bytes from `buffer` at position `pos`, and all three must
be aligned; `dma_write(pos, iov)`, like the read, writes several
buffers. The buffer comes from
[`temporary_buffer<char>::aligned()`]{index="`seastar::temporary_buffer`!`aligned()`" .main},
which allocates memory at the alignment given, as `random-reads.cc`'s
`fill()` does:

```{.cpp include="examples/files-and-storage/random-reads.cc" snippet="write"}
```

`dma_write()` doesn't take ownership of the buffer, and has no overload
that takes a `temporary_buffer`: the disk reads the buffer while the
write is in flight, so the program must keep it alive until the
future resolves. `fill()` does, because `buf` lives in the coroutine's
frame, and each write is awaited before the next.

When `dma_write()`'s future resolves, the data is in the disk's hands,
but not necessarily on its storage: the disk may keep it in a cache of
its own, and lose it if the power goes out. Only
[`flush()`]{index="`seastar::file`!`flush()`" .main} promises that
everything written to the file so far is *durable*, that it will survive
a crash: it calls `fdatasync()`, which waits until the disk confirms
that the data is stored. That takes time, about 3 ms on the laptop,
for a write of 4 KB, and not much more for a megabyte, and it's a cost
that every database designs around, as Shoal's log will.

> **Options for tests.** Direct I/O pays off in a server that runs for
> long, with a cache of its own that is full. A test runs briefly, and
> its cache is empty, so with direct I/O, every read would go to the
> disk. Seastar's option
> [`--kernel-page-cache`]{index="command-line options!`--kernel-page-cache`"}
> turns direct I/O off, so that files are read and written through the
> page cache, and a test reads the files that it wrote moments ago from
> memory. A test that doesn't care about crashes can add
> [`--unsafe-bypass-fsync`]{index="command-line options!`--unsafe-bypass-fsync`"},
> which makes `flush()` return at once. With both, a file that a test
> writes and soon deletes may never reach the disk at all, potentially
> making the test significantly faster. ScyllaDB's
> test framework runs its servers with both options.

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
for a small file that records the number of shards. Each shard keeps an
append-only log of its own, and which shard's log holds a key depends
on the number of shards, so replaying the logs needs that number:

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

## File systems {index="file systems"}

The example program `random-reads.cc` wrote random bytes to its file.
Its first version filled the file with the letter `x`, and found single
reads three times slower, and large reads faster than the disk can
deliver them. The laptop mounts its btrfs file system with compression,
as Fedora does by default, and btrfs reads compressed data through its
own cache, and decompresses it, direct I/O or not: the program was
measuring the file system, not the disk. Random bytes don't compress.
(Shoal's log, which is text, compresses well, so this chapter's
measurements of Shoal put its directory where btrfs doesn't compress,
with `chattr +m`.)

Seastar works on any Linux file system that supports direct I/O, but it
doesn't get the same from all of them. What matters is whether a read
or a write is really asynchronous: whether the call that should only
start it returns at once, or waits, for a lock or for the disk, and
stops the shard. That depends on how each file system is implemented in
Linux, not on its format on the disk, and it changes from one kernel
version to the next. Seastar recognizes the common file systems,
[XFS]{.index}, [ext4]{.index}, [btrfs]{.index}, NFS and `tmpfs`, and
works around what it knows makes each of them wait:

- A write that makes a file longer must also change the file's size,
  and that's where file systems wait. In XFS, starting such a write
  waits for every other request to the same file in flight, with the
  shard stopped. In ext4, it waits even when it's alone. So Seastar sends a write that makes a file
  longer only when no other request to the file is in flight. And it
  avoids such writes where it can: it first makes the file long enough,
  with `ftruncate()`, so that the writes no longer change its size, and
  can be sent together. On ext4, it does this before every write that
  would make the file longer; on XFS, which can take one such write at
  a time, only when several of them wait.
- In XFS, a flush holds a lock of the file until the disk confirms, and
  starting another request to the same file meanwhile waits for it,
  sometimes for hundreds of milliseconds, with the shard stopped. So on
  XFS, Seastar sends a file's flush only when none of the file's other
  requests is in flight, and holds new ones until the flush ends.

On btrfs, and on any file system that it doesn't know, Seastar plays
safe, and assumes both problems: it handles writes that make a file
longer as on ext4, and flushes as on XFS.

These workarounds keep the shard from stopping, but they make requests
to a file that grows wait for each other.

btrfs has a problem that no workaround removes: it does direct writes
synchronously. The call that should only submit a write waits until the
write is done: on the laptop, about 80 µs for a write of 4 KB, and
sometimes a few milliseconds, with compression or without. Seastar
keeps that wait away from the shard. With its default reactor backend,
io_uring, the kernel finishes such a write in a worker thread of its
own. The `linux-aio` backend submits writes with the flag
`RWF_NOWAIT`, which makes the kernel refuse a write that would wait,
and Seastar makes the refused write again in a helper thread. (On
Linux 6.x, Seastar uses `RWF_NOWAIT` only for reads, so there, with
`linux-aio`, a write on btrfs does stop the shard.) So a Seastar program
runs on btrfs without stalls: `random-reads.cc`, which writes a
gigabyte, had none of a millisecond or more, with either backend. But
every write takes a detour through another thread, which costs time and
CPU. Seastar's tool `iotune`, which [chapter](#ch:io-scheduler) will
use to measure the disk, checks for this before it measures, and
refuses btrfs; XFS, ext4 and `tmpfs` pass its check.

A file system that compresses, or that otherwise changes what it writes, also
takes the I/O out of the control of Seastar's I/O scheduler
([chapter](#ch:io-scheduler)), which assumes that a request goes to the
disk as it is.

So which file system? Seastar's own documentation says that XFS is
known to work, that ext4 may work well too, and that a program should be
tested with the file system and kernel that it will run on. ScyllaDB's
setup script formats its data disks with XFS, and its documentation
recommends XFS as the file system that it's tested with most. Until 2017,
ScyllaDB refused to start on any other file system, outside its
developer mode; it stopped checking after Seastar learned to work with
ext4. Redpanda's documentation
requires XFS or ext4 for its data, and recommends XFS, and its tool
`rpk` warns when the data directory isn't on XFS. Both, then, recommend
XFS; ext4 works too, while btrfs is best kept for the rest of the
machine. For development, any file system will do, but what a program
measures there includes the file system, as `random-reads.cc` found.

## The system-call thread {index="system-call thread"}

Reads, writes and `flush()` reach the disk through the reactor backend,
which, as [chapter](#ch:how-seastar-runs-your-code) described, lets the
shard hand a request to the kernel, and go on running. Most other file
operations have no such form, at least none that Seastar uses, and
their system calls may wait: to read a directory or a file's metadata
from the disk, to free the blocks of a large file, or for the server of
a network file system. So Seastar doesn't make these calls in the
shard's thread, but in a helper thread, the shard's
[*system-call thread*]{index="system-call thread" .main}. These
functions, with the system calls that they make, are:

- `open_file_dma()` and `open_directory()`, which call `open()`
- `file_stat()`, `file_size()` and `file_type()`, which call `stat()`,
  and `file::stat()`, which calls `fstat()`
- `file_accessible()` and `file_exists()`, which call `access()`
- `file_system_space()`, which calls `statvfs()`
- `chmod()` and `chown()`, which call the system calls of the same names
- `rename_file()`, which calls `renameat2()`
- `remove_file()`, which calls the C library's `remove()`, which calls
  `unlink()`, or `rmdir()` for an empty directory
- `link_file()`, which calls `link()`
- `make_directory()`, `touch_directory()` and
  `recursive_touch_directory()`, which call `mkdir()`
- `file::list_directory()`, which calls `getdents64()`
- `file::truncate()`, which sets the file's size, with `ftruncate()`
- `file::allocate()`, which asks the file system to reserve disk blocks
  for a range of the file, perhaps past its end, without changing its
  size, and `file::discard()`, which frees the blocks of a range, both
  with `fallocate()`
- `file::ioctl()` and `file::fcntl()`, which call `ioctl()` and
  `fcntl()` on the file's descriptor, with any command
- `file::close()`, for a file that was open for writing, which calls
  `close()`

Starting another program, with `spawn_process()`
([chapter](#ch:outside-world)), uses the system-call thread too.

Linux's io_uring can open, rename and remove files asynchronously, but
Seastar doesn't use it for these.

Each shard has a system-call thread of its own, named `syscall-0`,
`syscall-1` and so on, after the shard. A call
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

With the functions listed at the beginning of this section, a Seastar
program rarely needs to make a blocking system call of its own.
ScyllaDB and Redpanda make blocking system calls themselves only in a
few places, and for a reason:

- At startup, when a short wait harms no one: ScyllaDB locks each of
  its directories with `lockf()`, for which Seastar has no function, so
  that a second ScyllaDB can't use them, and Redpanda writes its PID
  file with `stat()`, `unlink()` and `open()`.
- Where Seastar can't be used at all: in a signal handler, where only
  the simplest system calls are safe. Redpanda's crash recorder writes
  the description of a crash from one, so it opens its file with the
  system call `open()`, and makes it durable with the system call
  `fsync()`.

Seastar has no mechanism for adding other system calls to the
system-call thread. A program can do the same with a thread of its
own, as [chapter](#ch:outside-world) shows. Such a thread is also the right
place for long work, which would hold up the system-call thread, since
it makes one call at a time. Redpanda starts one for its Kerberos
authentication, which uses a library that blocks, and ScyllaDB for
training the dictionaries that compress its RPC messages, which takes
long.

The metric `reactor_io_threaded_fallbacks` counts the system-call
thread's requests, with a label, `reason`, that tells file operations
from process operations.

## File streams {index="file streams"}

[`make_file_output_stream()`]{index="`seastar::make_file_output_stream()`" .main},
from `<seastar/core/fstream.hh>`, makes an output stream that writes a
file, as the file of the shard count, above, is written. It's the same
`output_stream<char>` class as a socket's. Here, the stream collects
the writes in aligned buffers of its own, and writes them to the file
with `dma_write()`.

Its counterpart,
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
option `appendfsync` decides. With `appendfsync always`, Redis flushes the file
before it replies to a command that changed something, so an
acknowledged change is never lost. With `appendfsync everysec`, the default, it
flushes once a second, and a crash of the computer loses up to a second
of acknowledged changes. With `appendfsync no`, it leaves the decision to the
kernel.

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
the `always` option, a changing command's reply is ready only after the commit.
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

A crash in the middle of a commit can leave the end of the file damaged,
in two ways. If Shoal is killed with `kill -9` between two of the
commit's writes, the commit's last request is cut short. A power failure can do worse: until
the flush, the disk may store the commit's blocks in any order, so it
may keep a later block without an earlier one. The missing block reads
as zeros, which the replay skips, so it goes on in the middle of a
request.

Either way, the replay comes to bytes that it can't parse, and it must
stop there, since in the middle of a request, it can't tell where the
next one begins. Shoal then cuts the file after the last whole request,
and logs how many bytes it cut. It must cut them: otherwise its new
commits would follow the damaged bytes, and the next replay would stop
at them again, and lose the new commits too.

Redis does the same for a request cut short at the end of its file,
but refuses to start if its file is damaged anywhere else, and offers a
tool to repair it. Shoal can't tell the two apart: it cuts whatever
follows the first damage, so damage in the middle of the file would
lose the good requests after it too. Exercise 3 gives the log what it
needs to tell them apart.

### Stopping, and crashing {index="clean shutdown"}

Shoal has stopped cleanly since version 7, in
[chapter](#ch:clean-shutdown), which said that this would matter more
once Shoal writes to disk. Now it does. With `everysec`, acknowledged
changes may wait in the tail for up to a second, and the clean shutdown
saves them: version 10's `store::stop()`, which `sharded<store>::stop()`
calls on every shard as Shoal stops, ends the periodic commits, and
closes the log, which commits what's left. A `kill -9`, or a crash,
gets no such chance, and loses the changes made
since the last commit. Redis with `everysec` loses less to a `kill -9`:
before it replies, it writes its log with `write()`, which leaves the
data in the kernel's memory, safe from a crash of Redis itself, and
only a crash of the whole computer loses the last second. Shoal's
uncommitted tail is in Shoal's own memory, and a write that would put it
in the kernel's would be a trip to the disk, since the log bypasses the
kernel's cache. Exercise 5 measures what each loses.

With the `always` option, nothing acknowledged is lost: version 10's tests kill Shoal with
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
`everysec`, and more than two thirds with the `always` option, and Shoal ends up a little
slower than Valkey with either. Group commit is what keeps the `always`
option usable: one client without a pipeline gets 337 `SET`s a second, a
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
591,000 `SET`s a second with `everysec`, and 359,000 with the `always` option. So
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

Like a stream, a `file` object is a handle to an implementation, a class that
derives from [`file_impl`]{index="`seastar::file_impl`" .main}, from
`<seastar/core/file.hh>`, and a program can put its own there: a file
that compresses, that encrypts, that counts, or that checks. Seastar's
[`layered_file_impl`]{index="`seastar::layered_file_impl`" .main}, from
`<seastar/core/layered_file.hh>`, is a convenient base, which holds the
file it wraps, and copies its alignments; it forwards nothing, so the
wrapper implements every operation of `file_impl`, most of them as a
line that calls the same operation on the wrapped file. To
use one, make a `file` object from it, with `file(make_shared<my_file>(...))`,
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
   writes. What do Shoal's clients see, with the `everysec` and the
   `always` options? Then make Shoal stop accepting changes after its log fails,
   with an error reply to each, as ScyllaDB stops serving after a disk
   error.

5. With `--appendfsync everysec`, write a client that sends `SET`s as fast
   as it can, counting those acknowledged, and kill Shoal with `SIGKILL`
   in the middle. After a restart, how many of the acknowledged `SET`s
   are missing, and how much time's worth is that? Do the same to Valkey,
   started with `--appendonly yes`. What would Shoal lose if, instead of
   Shoal, the computer crashed?
