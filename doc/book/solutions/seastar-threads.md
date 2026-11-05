# Seastar threads {#sol:seastar-threads}

## Exercise 1

A coroutine can't use `deferred_stop`: its destructor would have to
wait for `stop()`, and a destructor can't be a coroutine, so it can
only wait with `get()`, in a Seastar thread. The coroutine must stop its
services itself, on every path, and [chapter](#ch:coroutines)'s rule,
no `co_await` in a `catch` block, makes that clumsy: the exception must
be kept in a `std::exception_ptr` while the services stop, and thrown
again at the end:

```{.cpp include="examples/solutions/seastar-threads-ex1.cc" snippet="run"}
```

It prints the same as `deferred.cc`. Each service that started needs a
`try` block of its own, so that a failure while starting the next one
still stops it, and the nesting grows with every service: ScyllaDB's
`main()`, with more than a hundred services, shows why it runs in a
Seastar thread instead. [Chapter](#ch:clean-shutdown)'s
`with_stoppable()` is another way, for one service at a time, with a
function that runs while the service exists.

## Exercise 2

In a coroutine, the loops stay as they were, and only the yield
changes, to `co_await coroutine::maybe_yield()`, from
`<seastar/coroutine/maybe_yield.hh>`:

```{.cpp include="examples/solutions/seastar-threads-ex2.cc" snippet="flat"}
```

With the test of one number in a function of its own, which yields,
that function must become a coroutine too. It can wait only by
suspending, so it returns a `future<bool>` rather than a `bool`, and its
caller must wait for it with `co_await`, so the caller must be a
coroutine as well:

```{.cpp include="examples/solutions/seastar-threads-ex2.cc" snippet="split"}
```

The program runs the chapter's version, in a Seastar thread, and then
the two coroutines, each while a heartbeat notes how late it comes. The
best of three runs:

```
in a Seastar thread: 348513 primes in 872 ms; the heartbeat was up to 0.0 ms late
in a coroutine: 348513 primes in 873 ms; the heartbeat was up to 0.1 ms late
with a coroutine for each number: 348513 primes in 908 ms; the heartbeat was up to 0.1 ms late
```

All three left the shard free: the heartbeat was never late by more
than a fraction of a millisecond. The coroutine with the loops in it
took as long as the thread. Splitting off the test cost 35 milliseconds
for five million calls: about 7 nanoseconds a call, for the coroutine's
frame, allocated and freed on every call, and its future. That's little
here, 4%, since the average test of a number does more work than that,
but it's paid on every call, and the deeper the function that yields,
the more functions above it must become coroutines and pay it too. In a
Seastar thread, the test can stay an ordinary function that returns a
`bool` and calls `thread::maybe_yield()`, at any depth, for nothing.
