# Futures and promises {#ch:futures-and-promises}

[Chapter](#ch:coroutines) introduced futures, and then used them only
through `co_await`: a coroutine awaits a future, and gets its result, or
its exception. That's how Seastar code uses futures most of the time. But
a future is also an object, which a program can keep, examine, pass
around and create, and sometimes it needs to. This chapter looks at
futures up close: what the future of a coroutine holds when the call
returns, how to make a future without a coroutine, what makes a future
available, how many coroutines can wait for the same result, what happens
to a future that nobody waits for, and what all of this costs. Then Shoal
uses what we learned to serve requests up to 10% faster, and we see how
much that depends on the compiler's optimizer.

## A future, up close

### What a coroutine returns

As [chapter](#ch:coroutines) explained, calling a coroutine runs its body
right away, until the coroutine first suspends or ends, and only then
returns its future. So when the call returns, the future may be in any of
the three states that a future can be in. Each of these three coroutines
leaves it in a different one:

```{.cpp include="examples/futures-and-promises/states.cc" snippet="coroutines"}
```

A future's `available()` method says whether it holds its result or an
exception, and `failed()` says whether it holds an exception:

```{.cpp include="examples/futures-and-promises/states.cc" snippet="describe"}
```

The program calls the three coroutines, describes their futures, and only
then awaits them:

```{.cpp include="examples/futures-and-promises/states.cc" snippet="main"}
```

It prints:

```
answer_later() starts
answer(): available
no_answer(): failed
answer_later(): not available
answer() gives 42
no_answer() throws: no answer
answer_later() resumes
answer_later() gives 42
```

`answer_later()` printed its first line before the call returned, and
then suspended in `sleep()`, so its future wasn't available yet. The other
two never suspended, so their futures were already available, or failed,
when the calls returned, and awaiting them didn't suspend `main()`'s
coroutine. (Except, as [chapter](#ch:how-seastar-runs-your-code)
explained, when its task quota happens to be used up.)

Note also that calling `no_answer()` didn't throw, even though its body
did. A coroutine catches every exception that escapes its body, and
stores it in its future, so the exception appears only when someone
awaits the future, and then it's thrown in the coroutine that awaits it.

Seastar's function names call an available future *ready*, as in
`make_ready_future()` below. The two words mean the same.

A future also has a `get()` method, which returns the value of an
available future, or throws its exception. Don't call it on a future that
isn't available yet: a coroutine can't wait for anything without
suspending, so `get()` fails, and exercise 1 shows how badly. The one
place where `get()` can wait is a Seastar thread
([chapter](#ch:threads)). In a coroutine, use `co_await`.

### A future is used once

Note the `std::move()` in `co_await std::move(a)`. A Seastar future can be
awaited only once, and awaiting it takes the result out of it. A future
can be moved, but not copied, and `co_await` accepts only a future that is
moved into it. Writing `co_await a` doesn't compile, and the compiler's
complaint, about a member called `await_ready` that a future doesn't
have, isn't much help. Adding `std::move()` makes it compile, and also
makes it clear that `a` is used up: after the `co_await`, it holds
nothing, and must not be used again. For a result that many want to wait
for, Seastar has a different kind of future, `shared_future`, which you
will meet later in this chapter.

The program also shows that an asynchronous function doesn't wait for
anyone to await its future before doing its work. `answer_later()` started
when it was called, and its timer ran while the program was busy
describing futures. A future is only the way to learn the result. A
coroutine can start several operations, keep their futures, and only then
await them, and the operations proceed at the same time:
[chapter](#ch:concurrency-within-a-shard) builds on this.

## Futures without coroutines

A function that returns a future doesn't have to be a coroutine. If it
has the result at hand, it can return an available future, made by
`make_ready_future<T>(value)`; `make_ready_future<>()`, without a value,
makes an available `future<>`. In the same way,
`make_exception_future<T>(e)` makes a failed future, holding the
exception `e`, which may be an exception object or a
`std::exception_ptr`, and in a `catch` block,
`current_exception_as_future<T>()` makes a failed future holding the
exception being handled. A function can also return a future that it got
from another asynchronous function, as Shoal's `execute()` did in
[chapter](#ch:how-seastar-runs-your-code), returning `write()`'s future.

Why not just write a coroutine? Because this is cheaper: such a function
doesn't allocate a frame. Later in this chapter we measure how much
cheaper, and Shoal puts it to use.

But such a function must keep a rule that a coroutine keeps
automatically: it must report its errors in its future, not by throwing
them. A coroutine can't throw at its caller, because every exception from
its body ends up in its future. An ordinary function that returns a
future has no such protection: if it throws, its caller gets an exception
instead of a future. A coroutine that awaits the call right away won't
notice the difference, because in both cases the exception is thrown in
it, and its `try` catches both. But a caller that doesn't, because it
keeps the future to await it later, or drops it, or checks it with
`coroutine::as_future()` precisely to avoid exceptions, finds an exception
thrown at a call where it didn't expect one. So a function that returns a
future either doesn't throw, or catches its exceptions and returns them
in a failed future, with `make_exception_future()` or
`current_exception_as_future()`. Seastar's own functions keep this rule,
and many of them are declared `noexcept` to say so.

When you call a function that may not keep it, such as a function that
your code received as a parameter, call it through `futurize_invoke(f,
args...)`, which turns an exception that `f` throws into a failed future.
It also turns a result that isn't a future into an available future, so
generic code can call functions that return either. For such code,
`futurize_t<T>` names the type that `futurize_invoke()` returns, which is
`future<T>` for a type `T` that isn't a future, and `T` itself for a
future, and `is_future<T>` is true only for future types.

Two more small tools convert futures. `f.discard_result()` turns a
`future<T>` into a `future<>` that becomes available when `f` does,
dropping the value but keeping the exception, for code that expects a
`future<>`. And `as_ready_future(v)` is `make_ready_future()` with the
type taken from its argument.

## Promises

A future that isn't available yet has a *promise* at its other end. The
future is the end that reads the result, and the promise is the end that
writes it. A `promise<T>` gives its future, once, with `get_future()`,
and later fulfills it with `set_value()`, or fails it with
`set_exception()`. Every asynchronous function in Seastar works this way
underneath. A coroutine has a promise too, hidden in its frame, which
`co_return` fulfills. The network code keeps promises for the coroutines
that wait for data, and the timers keep promises for the coroutines that
sleep.

You need a promise of your own when the result comes from code that
doesn't return futures, such as a callback. Here is a `sleep()` of our
own, built on Seastar's `timer`, which calls a function when it expires
([chapter](#ch:time)):

```{.cpp include="examples/futures-and-promises/promise.cc" snippet="my-sleep"}
```

The promise and the timer are local variables of the coroutine, so they
live in its frame for as long as it waits. `co_await` finds the promise's
future unavailable, and suspends the coroutine. When the time is up, the
timer calls its function, which calls `set_value()`. That makes the
future available, and, as [chapter](#ch:how-seastar-runs-your-code)
described, puts the coroutine that waits for it in the reactor's queue of
tasks. When the reactor gets to it, `my_sleep()` resumes, and ends, which
fulfills its own promise, and puts the coroutine that awaits
`my_sleep()` in the queue, and so on, up the chain of coroutines that
wait for each other.

This is how Seastar waits without blocking, and without threads. Nothing
actually waits, in the sense that a thread waits. A suspended coroutine is
just a frame in memory, which the future it awaits remembers, and the
code that fulfills the promise, a timer here, a network event elsewhere,
is what puts the coroutine back to work.

A promise and its future are cheap. Unlike `std::promise` and
`std::future`, they don't share a state allocated on the heap. Each of
them points to the other, and when either one moves, it updates the
other's pointer. A value set before anyone waits is stored in the future
itself, and when a coroutine awaits a future that isn't available yet,
the promise learns which coroutine to put in the queue later. Like
everything else on a shard, futures and promises are used by one thread
only, so none of this needs locks or atomic operations.

### Broken promises

A promise should be kept: something must eventually call `set_value()`
or `set_exception()`. A promise destroyed without either is *broken*, and
then its future doesn't wait forever, but fails, with the exception
`broken_promise`. This function breaks its promise:

```{.cpp include="examples/futures-and-promises/promise.cc" snippet="broken"}
```

and awaiting its future

```{.cpp include="examples/futures-and-promises/promise.cc" snippet="broken-call"}
```

prints "broken() fails: broken promise". A broken promise is always a
bug, usually an object that held a promise and was destroyed while
someone still waited for it, and the exception at least makes the bug
visible.

To pass the result of a future to a promise, `f.forward_to(std::move(p))`
gives `p` the value or the exception of `f`, when `f` has one. It is
useful when an object keeps a promise that someone waits for, and the
result comes from another asynchronous function.

## Many waiters

A future has only one reader. When many coroutines need the same result,
Seastar has `shared_future<T>`, made from an ordinary future. Any number
of coroutines can call its `get_future()`, and each gets a future of its
own. When the original future becomes available, all of them become
available, each with a copy of its value, or fail with its exception. A
`shared_future` can itself be copied, and all copies share the same
result. Since every waiter gets a copy of the result, its type must be
copyable.

A common use is to load something only once. Suppose that loading the
value of a key is slow, and that many clients may ask for the same key at
the same time. Each of them would start loading it, unless the loader
remembers the loads it has started. This loader remembers the
`shared_future` of every key it has started to load, so a request for a
key that is being loaded waits for the same load, and a request for a key
that was loaded already gets an available future at once:

```{.cpp include="examples/futures-and-promises/shared.cc" snippet="loader"}
```

The future of `load()` becomes a `shared_future` when it's stored in the
map. Four coroutines ask for two keys at the same time, and when they're
done, a fifth asks for one of the keys again:

```{.cpp include="examples/futures-and-promises/shared.cc" snippet="ask"}
```

```{.cpp include="examples/futures-and-promises/shared.cc" snippet="main"}
```

The output:

```
1 got the value of x
3 got the value of x
2 got the value of y
4 got the value of y
5 got the value of x
2 loads in 100ms
```

There were two loads, not five, and since they ran at the same time, all
five requests together took as long as one load. Exercise 2 shows a
weakness of this loader: what happens when a load fails?

When the result comes not from a future, but from an event, use
`shared_promise<T>`, a promise with any number of futures, which its
`get_shared_future()` gives. For example, coroutines that must wait until
the program finishes starting up can wait for a `shared_promise<>`:

```{.cpp include="examples/futures-and-promises/shared.cc" snippet="wait-until-ready"}
```

and the code that starts up fulfills it when it's done:

```{.cpp include="examples/futures-and-promises/shared.cc" snippet="ready"}
```

This prints "starting up", and 100 ms later, "1 is running", "2 is
running" and "3 is running". A `shared_promise` keeps its value, so a
coroutine that calls `get_shared_future()` after `set_value()` gets the
value too, without waiting for anything. Exercise 3 builds something that wakes up its waiters
more than once.

The futures that `get_future()` and `get_shared_future()` give are
independent of each other, so one waiter can stop waiting without
affecting the others. Both functions accept a timeout, after which that
waiter's future fails with `timed_out_error`, or an `abort_source`, with
which other code can cancel that waiter's wait. [Chapter](#ch:time)
explains both.

## Futures that nobody waits for

### Dropping a future

What happens if a program calls an asynchronous function, and ignores the
future it returns? First, the compiler warns, because `future` is
declared `[[nodiscard]]`. GCC says:

```
warning: ignoring returned value of type 'seastar::future<int>',
declared with attribute 'nodiscard' [-Wunused-result]
```

and Clang says something similar. Usually, ignoring a future is a
mistake: the code forgot to `co_await` it, and goes on before the
operation has finished. When it's deliberate, as in the echo server of
[chapter](#ch:tutorial-introduction), which served each connection in the
background, writing `(void)` before the call tells the compiler so.

Dropping the future doesn't stop the operation. The operation goes on
without it, and there is just nobody to learn its result. This program
drops the future of a coroutine that sleeps and then fails:

```{.cpp include="examples/futures-and-promises/ignored.cc" snippet="fail-later"}
```

```{.cpp include="examples/futures-and-promises/ignored.cc" snippet="main"}
```

It prints (with the warning shortened):

```
dropped the future
fail_later() is still running
WARN  2026-11-07 11:16:24,349 [shard 0:main] seastar - Exceptional future
ignored: std::runtime_error (nobody is listening), backtrace: 0x5851e8 ...
done
```

`fail_later()` went on after its future was dropped, and when it failed,
its exception had nowhere to go. Seastar doesn't let such an exception
disappear silently: when a failed future, or a promise holding an
exception that no future will ever read, is destroyed, Seastar logs this
warning, with the exception and a backtrace. Here the warning came 10
milliseconds after the future was dropped, when the coroutine failed and
its frame, with its promise, was destroyed.

If dropping a future doesn't stop an operation, what does? Nothing does
by force: Seastar can't interrupt a coroutine. An operation that should be
possible to cancel must be written to check, at the points where it
waits, whether someone asked it to stop, which is what the abort sources
of [chapter](#ch:time) are for. And an operation that runs in the
background must be waited for before the objects it uses are destroyed,
and before the program exits, as exercise 4 shows; that's the subject of
[chapter](#ch:object-lifetime) and [chapter](#ch:errors-and-shutdown).

### "Exceptional future ignored"

This warning always means a bug, even when the bug is harmless: an error
happened, and nobody handled it. It has two usual causes:

- **A background operation that failed,** as above. There is no caller to
  handle its errors, so they must be handled in the operation itself. The
  echo server did that with `handle_exception()`, and
  [chapter](#ch:concurrency-within-a-shard) explains how this works, and
  the other ways to do it.
- **A failed future that the program examined, but didn't consume.**
  [Chapter](#ch:coroutines) checked futures with `coroutine::as_future()`,
  and called `ignore_ready_future()` on those that failed. Without it,
  destroying them would have logged this warning. `ignore_ready_future()`
  says "I know this future may have failed, and I chose to ignore it". To
  handle the exception rather than ignore it, take it out of the future
  with `get_exception()`, which returns it as a `std::exception_ptr`.

The backtrace shows where the failed future or promise was destroyed,
which, as here, is often not where the error happened. Its numbers are
addresses in the program, which [chapter](#ch:debugging-and-profiling)
shows how to turn into function names.

One more exception that you may meet in such warnings is
`nested_exception`. It holds two exceptions, when code that runs after a
failed operation, to clean up, fails too. `finally()`, a function of the
continuation style ([chapter](#ch:continuations)), reports such double
failures this way.

## What futures cost

How much do futures and coroutines cost? This program calls four
functions a hundred million times each: an ordinary function for comparison, a
function that returns an available future without being a coroutine, a
coroutine that returns without waiting, and a coroutine that waits once,
with `yield()`:

```{.cpp include="examples/futures-and-promises/cost.cc" snippet="functions"}
```

It calls them from a coroutine, which awaits each future:

```{.cpp include="examples/futures-and-promises/cost.cc" snippet="loop"}
```

`[[gnu::noinline]]` keeps the compiler from copying the functions into
the loop, as it couldn't if they were in another source file. Built with
`-O2`, on the laptop of [chapter](#ch:tutorial-introduction), with its
Intel Core Ultra 7 258V, each call took:

| The function | GCC 16 | Clang 22 |
|---|---|---|
| An ordinary function | 0.4 ns | 0.4 ns |
| Returns `make_ready_future()` | 2.0 ns | 1.6 ns |
| A coroutine that doesn't wait | 8.0 ns | 3.3–3.8 ns |
| A coroutine that waits once | 25 ns | 27 ns |

(Each number comes from at least three runs, which agreed within 4%,
except where the table shows a range.)

Creating and awaiting an available future costs less than two
nanoseconds more than an ordinary call. A coroutine adds its frame, which must be
allocated and freed, and set up. Seastar's memory allocator makes that
fast, but the coroutine still costs two to four times as much as an
available future, depending on the compiler. And a coroutine that waits,
even for a moment, costs much more: `yield()` suspends it, and then the
caller suspends too, because the future it awaits isn't available. The
reactor resumes the callee from its queue of tasks, and when the callee
ends, it puts the caller in the queue, and resumes it too. These two
trips through the queue add about 20 nanoseconds more.

All of these are small numbers. A Shoal request takes about 250
nanoseconds, so a coroutine call adds a few percent to it, and only code
that makes many calls per request needs to care. So write coroutines by
default, and make an exception only for code that is called very often,
and that usually has its result at hand. For such code, the usual
technique is to split a function in two: a *fast path*, an ordinary
function that handles the common case and returns an available future,
and a *slow path*, a coroutine that it calls only when it must wait.
Seastar's own `output_stream::write()` is written this way: when the data
fits in the stream's buffer, which is almost always, it copies the data
and returns `make_ready_future<>()`, and only otherwise calls a function
that waits.

(Newer compilers can sometimes avoid allocating a coroutine's frame, when
the coroutine is awaited right after the call, by putting the frame
inside the caller's frame. Seastar's `future` is marked to allow Clang to
do that, from Clang 23 on. We used Clang 22, which doesn't.)

### Why optimization matters

[Chapter](#ch:case-for-seastar) said that Seastar's templates are cheap
only when the compiler optimizes them. Here is how much that matters.
This is `cost.cc` again, compiled at each optimization level. Seastar's
library was compiled at `-O2` every time, so only the optimization of the
program's own code changed. The columns are the four functions of the
first table, in the same order, and four runs of each agreed within 7%:

| | Ordinary function | Available future | Coroutine, no wait | Coroutine that waits |
|---|---|---|---|---|
| GCC `-O0` | 1.1 ns | 42 ns | 85 ns | 164 ns |
| GCC `-O1` | 0.4 ns | 2.2 ns | 8.1 ns | 27 ns |
| GCC `-O2` | 0.4 ns | 2.0 ns | 7.9 ns | 25 ns |
| GCC `-O3` | 0.4 ns | 2.0 ns | 4.6 ns | 26 ns |
| Clang `-O0` | 0.8 ns | 38 ns | 83 ns | 180 ns |
| Clang `-O1` | 0.4 ns | 1.7 ns | 3.7 ns | 28 ns |
| Clang `-O2` | 0.4 ns | 1.6 ns | 3.3 ns | 27 ns |
| Clang `-O3` | 0.4 ns | 1.6 ns | 3.3 ns | 27 ns |

Without optimization, awaiting an available future costs about twenty
times as much, a coroutine that doesn't wait ten to twenty-five times as
much, and one that waits six or seven times as much. Any optimization at
all brings the costs down to nearly those of `-O2`. (GCC makes the
coroutine that doesn't wait cheaper still at `-O3`.)

Why such a difference? Most of what a future does isn't in Seastar's
compiled library, but in its headers, as templates and inline functions,
which are compiled as part of your program, with your program's options.
And it's built in layers of small functions. A `co_await` on an available
future creates an *awaiter*, which asks the future whether it's
available, which asks the future's state. Then the awaiter takes the
value out of the future, through its state and the wrapper that holds the
value, and the empty future, its state and the wrapper are destroyed.
Each of these functions is only a few instructions. Without optimization,
each is compiled separately, and called with its arguments in memory:
exercise 5 shows them in a profile. With optimization, the compiler
*inlines* them: it copies each small function into its caller, until the
whole `co_await` is one piece of code, and then it simplifies that piece
as a whole. A check whose answer is now known disappears: right after
`make_ready_future()`, the future is known to be available. A value moved
from one object to another goes through a register instead of memory. A
stack of small function calls becomes a handful of instructions.

But compilers don't inline everything. They inline a function only if
it's small enough, and stop when the function they inline it into grows
too large, so that the program, and the time to compile it, don't grow
too much. Seastar code, made of so many layers, often reaches these
limits. ScyllaDB's developers found this out twice. In 2018, they found
that GCC was too conservative in large source files, and raised one of
its limits, the parameter `inline-unit-growth`, to 300. In 2020, they
found that ScyllaDB built by Clang was 20% slower than built by GCC.
Raising Clang's *inline threshold*, its main limit, to 2500, brought the
gap down to 3%. They found the value by trial: with 5000, one of
ScyllaDB's source files never finished compiling, and with 1250, ScyllaDB
was 10% slower. ScyllaDB's release build still uses
`-mllvm -inline-threshold=2500`, and the next section shows what it does
to Shoal.

Inlining also stops at the edge of Seastar's compiled library. Its
memory allocator, for example, and the reactor's queue of tasks, are
compiled into the library, and can't be inlined into your code, unless
the program is built with *link-time optimization* (LTO), which optimizes
the library and the program together. *Profile-guided optimization*
(PGO) goes further: it tells the compiler which code is hot, from a
profile of the program at work. ScyllaDB's release build uses both, and
[chapter](#ch:deploying-and-tuning) explains them.

## Shoal, version 3

Shoal's request reader is such code.[^shoal-v3] It reads every line of
a request with `read_line()`, and every string with `read_exactly()`, so
a `SET` request makes seven of these calls, and in version 2 each is a
coroutine. But with clients that pipeline their requests, as the
benchmarks do, a request has usually arrived whole, so the data that each
call needs is already in the reader's buffer, and none of the coroutines
waits.

[^shoal-v3]: The source of this version is in `shoal/v3/`.

Version 3 gives both functions a fast path. `read_line()` looks for the
line in the buffer, and if it's there, returns it in an available future;
otherwise, it calls `read_line_slow()`, the coroutine that used to be
`read_line()`:

```{.cpp include="shoal/v3/resp.cc" snippet="read-line"}
```

Taking the line out of the buffer moved to a small function of its own,
which both paths use:

```{.cpp include="shoal/v3/resp.cc" snippet="take-line"}
```

```{.cpp include="shoal/v3/resp.cc" snippet="read-line-slow"}
```

`read_exactly()` does the same: when the buffer has all the bytes it
needs, it doesn't wait, and otherwise it calls `read_exactly_slow()`, the
coroutine that used to be `read_exactly()`:

```{.cpp include="shoal/v3/resp.cc" snippet="read-exactly"}
```

Nothing else changed. In particular, `read()`, which calls these
functions, is still a coroutine, so there is still a frame for each
request, and it `co_await`s their futures just as before.

What about the rule that a function that returns a future must not throw?
Both fast paths create an `sstring`, which allocates memory, and could
throw `std::bad_alloc` if there were none. That's acceptable here, because
`read()`, their only caller, is a coroutine that awaits each call right
away, so an exception thrown by the call lands in `read()`'s future, just
as a failed future would. A function for other callers would have to be
more careful.

The fast paths serve requests that arrived whole, and the slow paths those
that didn't. The tests of versions 1 and 2 send most requests whole, so
version 3 adds a test that sends the same two requests in two pieces,
split at every possible point, so that the reader must wait in the middle
of every line and every string. It passes against Shoal and against
`valkey-server`, and fails against a copy of Shoal that we broke on
purpose, by not copying the beginning of a string from the buffer in
`read_exactly_slow()`.

Measured as in [chapter](#ch:coroutines), with the same pipelines of 128
requests, and with Shoal built by GCC, in three runs each:

| Server, on one core | `SET` per second | `GET` per second |
|---|---|---|
| Shoal version 2 | 3,550,000–3,630,000 | 4,200,000–4,320,000 |
| Shoal version 3 | 3,890,000–4,000,000 | 4,440,000–4,570,000 |

Version 3 serves about 10% more `SET`s per second, and about 6% more
`GET`s, which have only two strings in a request, and so make fewer of
these calls. `perf stat`, a Linux tool that counts what the processor
does, showed that for each `SET`, version 3 executed about 2,850
instructions in user space, against about 3,780 in version 2: a quarter
fewer. But it used only 11% fewer processor cycles. The instructions that
the fast paths saved were cheap ones, so counting instructions would have
overestimated the gain. To know how much faster code is, measure its
time.

With Clang, the result depends on its inlining limit. Built by Clang 22
the same way, at `-O2`, version 3 was no faster than version 2: both
served 3,550,000–3,630,000 `SET`s per second. Built with Clang's inline
threshold raised to 2500, as in ScyllaDB, version 3 served
3,890,000–4,000,000 `SET`s per second, as many as with GCC. ScyllaDB's
other compiler options, `-O3` and `-march=x86-64-v3` (which lets the
compiler use the instructions of newer processors), did much less on
their own: 3,630,000–3,710,000. Whether a fast path pays off can depend
on the compiler's inlining decisions as much as on the code.

Is 10% worth the extra code? For a function on the path of every
request, yes. But this kind of optimization is worth making only where a
profile, or a measurement like this one, shows that the time goes. Most
of Shoal, and of any Seastar program, is better written as plain
coroutines.

## Exercises

1. In `states.cc`, call `c.get()` right after describing the futures,
   before awaiting any of them. What happens when you run the program?
   Why can't `get()` wait for the future, as `co_await` does?

2. Make the loader of `shared.cc` fail: let `load()` throw an exception
   the first time it's called, and succeed afterward. Ask for the same key
   three times, one after the other. How many loads are there, and what
   do the three requests get? Fix the loader so that it forgets a load
   that failed, and the next request for the key tries again. (Hint:
   `coroutine::as_future()`.)

3. Write a class `event`, with a method `wait()`, which returns a future,
   and a method `signal()`, which wakes up every coroutine that is waiting
   at that moment. A coroutine that calls `wait()` after a `signal()`
   waits for the next one. Build it on a `shared_promise<>`. (Hint: what
   happens if `signal()` replaces the `shared_promise` with a new one?)

4. In `ignored.cc`, make the program sleep only 5 milliseconds before
   printing "done", instead of 50. What does it print now? What happened
   to `fail_later()` and its exception? What does this mean for a program
   that leaves operations running in the background when it exits?

5. Build `cost.cc` with `-O0` and with `-O2`, run each under
   `perf record`, and look at `perf report`. Which functions take the time
   at `-O0`, but don't appear at all at `-O2`? Where did they go?
