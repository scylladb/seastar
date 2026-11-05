# Continuations {#sol:continuations}

## Exercise 1

A coroutine gets the values of `when_all_succeed()`'s tuple, or the
futures of `when_all()`'s, with a structured binding:

```{.cpp include="examples/solutions/continuations-ex1.cc" snippet="coroutines"}
```

It prints the same as `when-all.cc`. `co_await` on `when_all()`'s future
gives the tuple of futures, each available, and each still to be dealt
with: `get()` takes the value of the one that succeeded, and
`ignore_ready_future()` acknowledges the failure of the other.

## Exercise 2

`parse()` returns the failure instead of throwing it:

```{.cpp include="examples/solutions/continuations-ex2.cc" snippet="parse"}
```

and the empty string now goes through `show()`'s whole chain:

```
"21": 42
"21": done
"abc": not a number (stoi)
"abc": done
"": not a number (empty)
"": done
```

The rest of `parse()` needs no change: `std::stoi()` throws inside a
continuation, and `then()` turns whatever its continuation throws into a
failed future. Only code that runs before the first future exists can
throw past the chain.

## Exercise 3

```{.cpp include="examples/solutions/continuations-ex3.cc" snippet="count-down"}
```

With a hundred thousand, the recursion works: it took 2 to 5
milliseconds, and the heartbeat was as late as that, since it all ran in
one task. With ten million, the program crashes with a segmentation
fault; it does with a million too. In an optimized build, `then()` on an
available future calls its continuation at once, inside `then()`, so each
level of `count_down()` calls the next on the stack, and a million
levels need more stack than a shard's thread has. The program's output:

```
recursion, 100,000: 2.2 ms; the heartbeat was up to 1.1 ms late
repeat(), 10,000,000: 7.3 ms; the heartbeat was up to 0.3 ms late
```

`repeat()` counted down ten million in 7 milliseconds, with the
heartbeat never more than a third of a millisecond late: it runs each
turn after the previous one returns, in a loop rather than a recursion,
and checks for preemption between turns, so it yields when the task quota
is used up. In a debug build, the recursion wouldn't crash either:
`then()` schedules each continuation as a task there, even on an
available future, so the levels run one after another, from the
reactor's loop. That's the worst kind of bug: one that a debug build
can't find.

## Exercise 4

`ready()` becomes a coroutine that waits once, with `co_await yield()`,
before it returns. With ten million calls, on one shard:

```
ready(), which waits            26.5 ns per call
doubled with then()             38.4 ns per call
doubled in a coroutine          38.9 ns per call
```

When the future isn't available, the continuation is no cheaper than the
coroutine. `then()` must then keep its function until the future is
resolved, in a task that it allocates, much as the coroutine allocates its
frame, and both wait for the reactor to run them. The advantage of the
continuation is all in the fast path, which is why it suits only code
whose futures are usually available.

## Exercise 5

The obvious answer is a capture of the loop's function, a `mutable`
lambda with `n = size_t(0)`, which its continuation updates through a
reference. That version compiled, and answered every client "0 bytes".
`repeat_until_value()` calls its function, and if the function's future
isn't available, moves the function into a state of its own, on the heap,
and returns; the function's first copy, which the continuation refers to,
was a parameter of `repeat_until_value()`, and was gone when the read
completed. The count must live in an object that the function and the
continuation share:

```{.cpp include="examples/solutions/continuations-ex5.cc" snippet="count-bytes"}
```

The `lw_shared_ptr` ([chapter](#ch:object-lifetime)) is copied into each
continuation, so the count lives until the last of them is done, wherever
the function was moved. This is the same trap as the Lambda Coroutine
Fiasco: a reference into an object whose life someone else controls.
`do_with()` avoids it by giving the object a place of its own, which is
why continuation code uses it so often.
