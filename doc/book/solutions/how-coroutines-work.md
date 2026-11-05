# How coroutines work {#sol:how-coroutines-work}

## Exercise 1

The abort source's subscription becomes a member of the awaiter, next to
the timer, so it lives in the coroutine's frame while the coroutine
waits. Whichever fires first schedules the coroutine, and cancels the
other:

```{.cpp include="examples/solutions/how-coroutines-work-ex1.cc" snippet="abortable"}
```

A sleep of 10 milliseconds, and then one of a second, which a timer
aborts after 5 milliseconds:

```
slept after 10 ms
aborted after 5 ms
```

The exception is thrown by `await_resume()`, on the shard, as the
coroutine resumes, so the coroutine sees it as if the `co_await`
expression had thrown it, and can catch it there. Throwing from the
subscription's function instead wouldn't work: that function is called
from `request_abort()`, in whatever code requested the abort.

## Exercise 2

The program measures the allocations that a call makes until it first
waits:

```{.cpp include="examples/solutions/how-coroutines-work-ex2.cc" snippet="frames"}
```

With GCC, and with Clang:

```
inner(): 1 allocations, 128 bytes
outer(), awaiting inner(): 2 allocations, 312 bytes
local_before(): 1 allocations, 1144 bytes
```

(Clang allocated 304 and 1,136 bytes.) The callee's frame isn't part of
the caller's: calling `outer()` allocated two frames, each of its own
size, since `inner()` runs inside `outer()` until it first waits, and both
must survive that. And the array of `local_before()`, which isn't used
after the suspension, still takes its place in the frame, with both
compilers: they put every local variable whose life spans a suspension
point into the frame, and the array's life, its scope, spans it, although
nothing reads it afterwards. To keep large temporary data out of the
frame, put it in an ordinary function that the coroutine calls before it
waits, or in a scope that ends before the suspension.

## Exercise 3

The two generators yield ten million integers each, the buffered one in
batches of 256:

```{.cpp include="examples/solutions/how-coroutines-work-ex3.cc" snippet="generators"}
```

On one shard:

```
unbuffered: 9.2 ns per value
buffered: 7.9 ns per value
```

The buffered generator saves less than one might expect: a value from
the unbuffered generator costs a suspension and a resumption of each
coroutine, but these are direct transfers between the two, without the
reactor's queue, so they're cheap; the buffered one pays instead for
pushing each value into the vector, and for the consumer's taking it out.
For values that are expensive to move, the unbuffered generator, which
yields each value in place, would win.

## Exercise 4

The awaitable sends the shard a message that resumes the coroutine there:

```{.cpp include="examples/solutions/how-coroutines-work-ex4.cc" snippet="on-shard"}
```

```
caller: on shard 0
hop: starts on shard 0
hop: continues on shard 1
caller: after hop(), on shard 1
```

The caller never asked to move, but it continued on shard 1 too. The
coroutine `hop()` finished on shard 1, and fulfilled its promise there,
which scheduled its caller, the task waiting for the promise, on shard
1's reactor. So every coroutine up the chain continues on the wrong
shard, using its own shard's memory and data from another, which breaks
every rule of [chapter](#ch:shared-nothing). The first version of this
program didn't move back, and Seastar aborted: `main()`'s function ended
on shard 1, and Seastar's `stop()` must be called on shard 0. Besides,
the coroutine's frame, allocated on shard 0, is used, and finally freed,
on shard 1. The right way to run code on another shard is
`smp::submit_to()`, with a function that does the other shard's part,
and returns a future to the original shard.

## Exercise 5

```{.cpp include="examples/solutions/how-coroutines-work-ex5.cc" snippet="prefix"}
```

On a map of 100,000 keys, it prints `33334 keys start with "item:"`.
The generator can't yield to the reactor itself, since `std::generator`
is synchronous: its body can't `co_await`, because its consumer, a plain
`for` loop, can't wait for it. So the consumer yields, between the keys
that it takes. Note what that means: while the consumer waits in
`maybe_yield()`, the generator is suspended in the middle of its loop,
with an iterator into the map, and other tasks may run, and change the
map. An insertion leaves the iterator valid, but erasing the very key
that it points to would leave it dangling, so a real program would need
to guard against changes while it walks, as [chapter](#ch:how-seastar-runs-your-code)'s
`KEYS` had to.
