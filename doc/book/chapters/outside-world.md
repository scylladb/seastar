# Blocking code and the outside world {#ch:outside-world}

Everything so far ran on the shards, in tasks, which wait by giving the
shard back. Some code can't run that way at all: a library that blocks,
as [chapter](#ch:case-for-seastar) warned, a computation that runs for
seconds without a place to yield, or a whole existing program built on
threads. It must run in operating-system threads that aren't shards.
This chapter is about such threads: the ones that Seastar starts for
itself, the ones that a program starts, and how they and the shards
talk. It ends with Seastar's way to run other programs.

These are real operating-system threads, which run in parallel with the
shards, and may block. They have nothing to do with *Seastar threads*,
objects of the class `seastar::thread`, which, despite their name, run
on a shard, as fibers with stacks of their own, and are the subject of
[chapter](#ch:seastar-threads).

## Seastar's own threads {index="system-call thread"}

A Seastar process has more threads than shards. These are the threads
that exist when Shoal runs on two shards:

```
$ pidof shoal
727221
$ cat /proc/$(pidof shoal)/task/*/comm
shoal
shoal-ust
shoal-ust
reactor-1
syscall-0
syscall-1
iou-wrk-727221
```

`shoal`, the program's first thread, is shard 0's reactor, and
`reactor-1` is shard 1's. The two `-ust` threads are LTTng's, the
tracing library ([chapter](#ch:observability)), which Seastar includes
unless it's configured with `--disable-lttng`.

`iou-wrk-727221` is the kernel's: a worker of io_uring. Each shard has
an io_uring of its own, and when the shard asks it for something that
can't be done without blocking, the kernel does it in a worker thread,
so that the shard doesn't wait. An `fdatasync()` always needs a worker,
and so does a direct write on btrfs, as
[chapter](#ch:files-and-storage) showed. The kernel starts a worker
when one is first needed, and more if blocking requests pile up, and
stops a worker that has been idle for five seconds, unless it's the
io_uring's last. A worker is named after the thread whose io_uring it
serves, here 727221, the process's id, which is also the id of its
first thread, shard 0's. So shard 0 had already needed a worker, as
Shoal started, and shard 1 not yet. After a few `SET`s with
`--appendfsync always`, which call `fdatasync()` on the shards that
own the keys, shard 1 had a worker too, named after `reactor-1`'s id,
and both workers were still there, idle, seven seconds later.

And each shard has a *system-call thread*, `syscall-0`
and `syscall-1`, which, as [chapter](#ch:files-and-storage) explained,
makes the system calls that have no form that doesn't block, such as
opening a file, renaming it, or starting a process, so that the shard
goes on running meanwhile. The system-call thread is Seastar's own: a
program can't give it code of its own to run. A program's blocking code
needs threads of its own.

## Threads that aren't Seastar's {index="alien thread"}

A thread that a program starts itself, with `std::thread`, is outside
Seastar, which calls it an *alien thread*. It can block: it isn't a shard, and
nothing waits for it. But it can't call anything of Seastar's that needs
a reactor, which is nearly everything: sleeping, timers, `submit_to()`,
even `this_smp_shard_count()`, which crashed the first version of the
example below. Its memory comes from the C library's `malloc()`, not
from a shard's ([chapter](#ch:memory)); memory of a shard that it frees
goes back to that shard, through the list that
[chapter](#ch:memory) described.

What an alien thread can do is send functions to the shards, with two
functions from `<seastar/core/alien.hh>`.
[`alien::run_on()`]{index="`seastar::alien::run_on()`" .main} sends a
function to a shard, to run there as a task, and returns at once; the
function must be `noexcept`, since nothing could receive its exception.
[`alien::submit_to()`]{index="`seastar::alien::submit_to()`" .main}
sends a function that returns a future, and returns a `std::future` of its
result, which the alien thread can wait for, blocking itself. This is the
one place where a Seastar program meets `std::future`, which
[chapter](#ch:coroutines)'s corner said has no place in it: an alien
thread may block, so it may wait for one. Both take an
[`alien::instance`]{index="`seastar::alien::instance`"}, which stands for
the Seastar program, from `app_template::alien()`:

```{.cpp include="examples/outside-world/alien.cc" snippet="outside"}
```

The shard that starts the alien thread must not block either, so it
doesn't wait for the thread with `join()`, which would block it until the
thread ends. Instead, the thread ends by sending its answer to the shard
with `run_on()`, which fulfills a promise there, and the shard waits for
the promise's future, as for any other. Only then does it call `join()`,
on a thread that is about to end:

```{.cpp include="examples/outside-world/alien.cc" snippet="start"}
```

On two shards, this prints `the shards' answers add up to 100`.

Seastar's support for alien threads came from Ceph, whose Crimson
project rebuilds Ceph's storage server on Seastar. Kefu Chai, of Red
Hat, added it in 2018, saying that "it helps when an application is
migrating to seastar": an existing program, made of threads, can move
to Seastar a part at a time, with its old threads sending work to the
shards.

### A worker for code that blocks {index="alien thread!worker"}

A common use of alien threads is to run code from libraries that block,
or that compute for a long time without yielding, and therefore can't
run directly on a shard. Seastar has no ready-made way to do this, but
programs usually build the same thing: a *worker*, an alien thread that
runs functions that the shards send it, and sends back their results.
Here is a simple one, written for this book, which runs one function at
a time, taking them from a queue that a mutex protects; at the end of
this section we'll see what the workers of real programs do
differently:

```{.cpp include="examples/outside-world/worker.cc" snippet="worker"}
```

`submit()` is a coroutine, on the shard. Its promise stays in its frame,
and the worker only gets the promise's address: when the function
returns, the worker sends the result to the shard with `run_on()`, and
the promise is fulfilled there, on its own shard:

```{.cpp include="examples/outside-world/worker.cc" snippet="submit"}
```

To stop, the worker first refuses new functions: from then on,
`submit()` fails. Then it finishes the functions already submitted,
which may come from any shard, and its thread's last act is to fulfill a
promise on the shard that made the worker, which `stop()` waits for,
without blocking. Only then does `stop()` call `join()`, on a thread
that is about to end:

```{.cpp include="examples/outside-world/worker.cc" snippet="stop"}
```

The program gives the worker a function that blocks for 300
milliseconds, and then one that counts the primes below five million,
the slow way, which takes almost a second and never yields. Then it calls the same two functions
on the shard itself. Meanwhile, a timer that fires every millisecond
counts how often the shard was free to run it:

```{.cpp include="examples/outside-world/worker.cc" snippet="use"}
```

```
in the worker: got 42 and 348513 primes, and the timer fired 1180 times
on the shard: got 42 and 348513 primes, and the timer fired 1 times
```

When the two functions ran in the worker, the shard stayed free, and
ran its timer through the whole second and a quarter that they took.
When they ran on the shard itself, the shard ran its timer only once,
after both had ended, and the stall detector reported a stall of more
than a second.

A thread inherits the CPU affinity of the thread that starts it, and a
shard's reactor is pinned to its core, so this worker runs on its
shard's core: when both have work, the kernel divides the core between
them. Real workers take care of this, and more. ScyllaDB's, which it
uses to train compression dictionaries for its RPC, stays on the shard's
core, but lowers its priority to the lowest, with `nice(19)`, so that
the reactor gets the core whenever it wants it, and blocks every signal
in the worker, so that signals go to Seastar's threads. Redpanda's,
which runs its Kerberos and OpenSSL code in an alien thread, also blocks
every signal, and
bounds its queue with a Seastar semaphore, so that the shards can't
submit more than it keeps up with. And it never makes the shard wait for
a lock, as `worker.cc` does, briefly, when its worker holds the mutex:
the queue is a lock-free one, from Boost, and the worker sleeps reading
an [*eventfd*]{index="eventfd"}, a counter in the kernel that one thread
can add to and another can wait on, which the shard adds to, without
blocking, when it queues a function. And Ceph's Crimson runs its
classic storage engine, BlueStore, unchanged, a whole subsystem built on threads, in a pool of
alien threads: a shard's calls go to the pool through a queue bounded by
a semaphore, all calls on one collection go to the same thread, so that
they stay in order, and BlueStore's own threads, which commit
transactions and write its log, call `alien::submit_to()` to reach the
shards.

## Running other programs {index="process"}

[`spawn_process()`]{index="`seastar::spawn_process()`" .main}, from
`<seastar/core/seastar.hh>`, starts another program, and returns a
[`process`]{index="`seastar::process`"}, from
`<seastar/util/process.hh>`, whose standard input, output and error are
Seastar streams ([chapter](#ch:networking)), and whose `wait()` returns
a future of how it ended. This one sorts three words:

```{.cpp include="examples/outside-world/process.cc" snippet="spawn"}
```

It prints:

```
pear
fig
apple
sort exited with status 0
```

Starting the program is one of the system calls that block, so
`spawn_process()` makes it in the system-call thread. Every process must
be waited for, with `wait()`, even one that was killed, or it stays a
zombie until Seastar's program ends.

## Exercises

1. Make `worker::submit()` wait, before it queues a function, while the
   worker already has 16 functions waiting, as Redpanda's worker does,
   with a semaphore of the shard's. Submit a hundred functions that each
   sleep 10 milliseconds, all at once, and check that no more than 16 ever
   wait in the queue.

2. Build a pool of four workers, in which every function has a key, and
   functions with the same key always go to the same worker, as Crimson's
   do, so that they run in the order they were submitted. Submit eight
   functions with each of four keys, each sleeping 50 milliseconds, and
   check both the order of each key's functions, and the total time.

3. In `alien.cc`, make the alien thread send 100,000 functions to shard
   0 with `alien::run_on()`, each incrementing a counter, and measure how
   long each takes to send and to run. Compare it with sending a function
   from one shard to another, which [chapter](#ch:talking-between-shards)
   measured.

4. Use `spawn_process()` to run `sh -c 'exit 3'`, and `sh -c 'kill -9
   $$'`, and print how each ended. What happens if the program doesn't
   exist?
