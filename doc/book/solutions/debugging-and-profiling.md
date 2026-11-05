# Debugging and profiling {#sol:debugging-and-profiling}

## Exercise 1

The command is in `examples/solutions/debugging-and-profiling-ex1.py`,
which is loaded after the chapter's `seastar.py`, with `source`. Since
it and the next exercise's command both visit every shard, the file has
a helper for that, `shards()`, which does what the chapter's
`seastar-task-queues` did: it switches to each thread, reads its
`seastar::local_engine`, and skips the threads that aren't shards, and
the shards it has seen. The command itself reads each shard's
`cpu_mem`:

```{.python include="examples/solutions/debugging-and-profiling-ex1.py" snippet="memory"}
```

On Shoal, started with `-c2 -m1G`, and idle:

```
(gdb) seastar-memory
shard 0: 512 MB at 0x600000000000, 499 MB free
shard 1: 512 MB at 0x601000000000, 501 MB free
```

The same command works on a core dump, since `cpu_mem` is in the
shards' memory, which the core dump keeps. In a `debug` build it
doesn't: as [chapter](#ch:memory) explained, Seastar's allocator isn't
used there, and `cpu_mem` is empty.

## Exercise 2

The command, in the same file, looks for the shard whose memory holds
the address, from `cpu_mem`'s `memory` and `nr_pages`, rather than
computing it from `cpu_id_shift`: that works whatever the layout, and
tells an address that's in no shard's memory, such as a stack's:

```{.python include="examples/solutions/debugging-and-profiling-ex1.py" snippet="shard-of"}
```

With a breakpoint in `pubsub::publish_here()`, as in the chapter, at
its second stop, in the coroutine's body:

```
(gdb) print channel
$1 = (const seastar::sstring &) @0x6000003f1190: "news"
(gdb) seastar-shard-of &channel
0x6000003f1190 is in shard 0's memory
(gdb) seastar-shard-of frame_ptr
0x600000185a40 is in shard 0's memory
(gdb) seastar-shard-of $sp
0x7ffcfcd6c288 is in no shard's memory: a stack, a global, or memory from outside Seastar's allocator
```

The channel's name and the coroutine's frame are both shard 0's, the
shard that runs the coroutine, as they should be; the stack pointer,
`$sp`, is in the thread's stack, which isn't Seastar's memory. And
`print channel` shows the name as a string, thanks to `seastar.py`'s
pretty printer.

## Exercise 3

We profiled Shoal 16 with `--appendonly yes`, as the chapter did
without it, and merged and cleaned its stacks with `clean_stacks.py`.
Recording the requests in the log took 16% of the samples, in Shoal's `record()`, and
of those, 83% were in the log's `append()`, and 15% in looking up the
key's old value. Of `append()`'s time, 63% went to `fmt::format_to()`,
which writes the numbers in each request's RESP, such as the `$3` before
a word of three letters, and 36% to copying the bytes into the log's
buffer:

```{.cpp include="shoal/v16/resp.hh" snippet="format-request"}
```

The commits, which write the log to the disk, took only 1.7%: the
writing itself is done by the kernel and the disk, which a profile of
the program's user space doesn't see, and Shoal only hands over its
buffers. So the log's cost, to Shoal's CPU, is mostly in preparing its
bytes, and most of that in formatting small numbers. That's the kind of
answer that a flame graph gives, and that a guess wouldn't have: a
faster way to turn a number into text would be the place to start.
