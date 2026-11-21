# Seastar threads and the outside world {#sol:threads}

## Exercise 1

The worker gets a semaphore of the shard's, with 16 units, and
`submit()` takes a unit before it queues the function, and keeps it, in
its frame, until the function's result has come back:

```{.cpp include="examples/solutions/threads-ex1.cc" snippet="room"}
```

The test submits a hundred functions at once, and the worker notes the
longest its queue ever got:

```{.cpp include="examples/solutions/threads-ex1.cc" snippet="test"}
```

It prints `sum 4950; at most 16 functions waited; 1012 ms`. The hundred
functions took a second, as a hundred sleeps of 10 milliseconds, one
after another, must; the semaphore only kept the other 84 waiting on the
shard, as coroutines suspended in `get_units()`, rather than in the
worker's queue. That's the point of the bound: what waits on the shard is
under the shard's control, where a semaphore of bytes, or a timeout, can
govern it ([chapter](#ch:coordination)), while what waits in the queue
is out of its reach.

## Exercise 2

The pool holds four of the chapter's workers, and the key chooses one:

```{.cpp include="examples/solutions/threads-ex2.cc" snippet="pool"}
```

The test submits eight functions with each of four keys, all at once,
and records, for each key, the order in which its functions' results
came back:

```{.cpp include="examples/solutions/threads-ex2.cc" snippet="test"}
```

It prints:

```
4 workers: in order: true; 402 ms
1 workers: in order: true; 1604 ms
```

Each key's functions ran on one worker, one after another, so they
ended in order, and their results came back in order too: a worker's
`run_on()` messages to a shard arrive in the order it sent them. The
four keys ran in parallel, on four workers, so the 32 functions took as
long as eight, 400 milliseconds, against 1.6 seconds on one worker. With
more keys than workers, keys share workers, and a key's functions wait
for the other keys' functions on their worker, which is the price of
keeping each key's order without any locks.

## Exercise 3

The alien thread sends the functions one after another, and the shard
waits until it has counted them all:

```{.cpp include="examples/solutions/threads-ex3.cc" snippet="send"}
```

On two shards, the best of three runs printed `sending: 158 ns each;
until all ran: 165 ns`: about 160 nanoseconds for each message, to send
it and to run it. That's much less than the microsecond that
[chapter](#ch:talking-between-shards) measured for a word sent from
shard to shard, one at a time, because that was a round trip, with a
reply to wait for, while `run_on()` only sends, and the alien thread
sends the next message without waiting. The shard takes the messages
that have arrived together, in each pass of its reactor's loop, so like
chapter 14's batches, they cost little each. Note also where the alien
thread ran: started by shard 0, it inherited shard 0's CPU affinity, and
shared its core, so the two took turns on it.

## Exercise 4

A coroutine can't use `deferred_stop`: its destructor would have to
wait for `stop()`, and a destructor can't be a coroutine, so it can
only wait with `get()`, in a Seastar thread. The coroutine must stop its
services itself, on every path, and [chapter](#ch:coroutines)'s rule,
no `co_await` in a `catch` block, makes that clumsy: the exception must
be kept in a `std::exception_ptr` while the services stop, and thrown
again at the end:

```{.cpp include="examples/solutions/threads-ex4.cc" snippet="run"}
```

It prints the same as `deferred.cc`. Each service that started needs a
`try` block of its own, so that a failure while starting the next one
still stops it, and the nesting grows with every service: ScyllaDB's
`main()`, with more than a hundred services, shows why it runs in a
Seastar thread instead. [Chapter](#ch:clean-shutdown)'s
`with_stoppable()` is another way, for one service at a time, with a
function that runs while the service exists.

## Exercise 5

The program runs each command, waits for it, and reports how it ended,
or the exception that starting it threw:

```{.cpp include="examples/solutions/threads-ex5.cc" snippet="run"}
```

It prints:

```
exited with status 3
killed by signal 9
failed to start: No such file or directory
```

`wait()` returns a `std::variant`: `process::wait_exited`, with the
program's exit status, or `process::wait_signaled`, with the signal that
killed it. A program that doesn't exist fails at once:
`spawn_process()`'s future fails with the error of `posix_spawn()`, the
system call that the shard's system-call thread made, as a
`std::system_error`.
