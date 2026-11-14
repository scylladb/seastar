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

What does a coroutine return, when we call it without `co_await`? As
[chapter](#ch:coroutines) explained, calling a coroutine runs its body
right away, until the coroutine first suspends or ends, and only then
returns its future. So when the call returns, the future may be in any of
the three states that a future can be in: *unavailable*, when the result
isn't there yet; *available*, when it holds the result; or *failed*, when
it holds an exception instead of a result, because the operation failed.
Each of the following three coroutines leaves it in a different one:

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

(Why `co_await std::move(f1)`, and not `co_await f1`? We'll see below, in
"A future is used once".) It prints:

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

This book calls a future that holds its result or its exception
"available", as Seastar's `available()` method does. But Seastar's
function names often say [*ready*]{index="`seastar::future`!ready"}
instead, as in `make_ready_future()`, which we'll meet below: a "ready"
future is an available one. And when a future becomes available, with
its result or its exception, we say that it
[*resolves*]{index="`seastar::future`!resolved"}, as Seastar's
documentation does: a function returns, for example, "a future that
resolves when" its work is done.

A future also has a `get()` method, which returns the value of an
available future, or throws its exception. Don't call it on a future that
isn't available yet: a coroutine can't wait for anything without
suspending, so `get()` fails, and exercise 1 shows how badly. The one
place where `get()` can wait is a Seastar thread
([chapter](#ch:threads)). In a coroutine, use `co_await`.

### A future is used once {index="`seastar::future`!used once"}

Note the `std::move()` in `co_await std::move(f1)`. A Seastar future can be
awaited only once, and awaiting it takes the result out of it. A future
can be moved, but not copied, and `co_await` accepts only a future that is
moved into it. Writing `co_await f1` doesn't compile, and the compiler's
complaint, about a member called `await_ready` that a future doesn't
have, isn't much help. Adding `std::move()` makes it compile, and also
makes it clear that `f1` is used up: after the `co_await`, it holds
nothing, and must not be used again. For a result that many want to wait
for, Seastar has a different kind of future, `shared_future`, which you
will meet later in this chapter.

A future moves its value too: out of the coroutine that produced it, into
the future, and on to the coroutine that awaits it. An exception thrown
on the way would have nowhere to go: the coroutine that produced the
value has already finished, and the one waiting for it hasn't resumed
yet. So Seastar declares the functions that do these moves `noexcept`
(for example, the future's own move constructor), moving the value must
not throw, and `future<T>` checks, with a `static_assert`, that the move constructor of
`T` is `noexcept`. If it isn't, the program doesn't
compile: "static assertion failed: Types must be no-throw move
constructible".

> **C++ corner: `noexcept` move constructors.**[]{index="C++ corner!`noexcept` move constructors"}[]{index="`noexcept`!move constructors"} A move constructor
> that the compiler writes is `noexcept` when the move constructors of all
> the members are, so most types have one without any effort. But a class
> whose move constructor is written by hand, without `noexcept`, doesn't:
>
> ```cpp
> struct name {
>     std::string value;
>     name(std::string v) : value(std::move(v)) {}
>     name(const name&) = default;
>     name(name&& other) : value(std::move(other.value)) {}
> };
> ```
>
> Declare such a move constructor `noexcept`, or better, don't write it,
> and let the compiler write it, either by not declaring it at all or by
> declaring it `= default`. The standard library cares about this too:
> when a `std::vector` grows, it moves its elements to their new place
> only if their move constructor is `noexcept`, and otherwise copies them
> (if they can be copied), which is slower.

The program also shows that an asynchronous function doesn't wait for
anyone to await its future before doing its work. `answer_later()` started
when it was called, and its timer ran while the program was busy
describing futures. A future is only the way to learn the result. A
coroutine can start several operations, keep their futures, and only then
await them, and the operations proceed at the same time:
[chapter](#ch:concurrency-within-a-shard) builds on this.

## Futures without coroutines {index="`seastar::future`!without coroutines"}

A function that returns a future doesn't have to be a coroutine. If it
has the result at hand, it can return an available future, made by
`make_ready_future<T>(value)`; `make_ready_future<>()`, without a value,
makes an available `future<>`. In the same way,
`make_exception_future<T>(e)` makes a failed future, holding the
exception `e`, which may be an exception object or a
[`std::exception_ptr`]{index="`std::exception_ptr`"}, and in a `catch` block,
`current_exception_as_future<T>()` makes a failed future holding the
exception being handled. A function can also return a future that it got
from another asynchronous function, as Shoal's `execute()` did in
[chapter](#ch:how-seastar-runs-your-code), returning `write()`'s future.

Why not just write a coroutine? Because this is cheaper: such a function
doesn't allocate a frame. Later in this chapter we measure how much
cheaper, and Shoal puts it to use.

Two more small tools convert futures. `f.discard_result()` turns a
`future<T>` into a `future<>` that becomes available when `f` does,
dropping the value but keeping the exception, for code that expects a
`future<>`. And `as_ready_future(v)` is `make_ready_future()` with the
type taken from its argument.

### Asynchronous functions and exceptions {index="exceptions!in asynchronous functions"}

An asynchronous function can report an error in two ways: it can return
a failed future, or it can throw, and return no future at all. A
coroutine always does the first: every exception that escapes its body
ends up in its future, so calling a coroutine never throws. An ordinary
function that returns a future can do either. This one, `store()`,
checks its argument and throws if it's bad, and otherwise returns the
future of a coroutine, `write_key()`, that does the work:

```{.cpp include="examples/futures-and-promises/throwing.cc" snippet="store"}
```

Whether the throw matters depends on the caller. A caller that awaits
the call right away, as in `co_await store(key)`, can't tell the
difference: either way, the exception is thrown in the caller, where a
`try` around the `co_await` can catch it. The trouble is with a caller that doesn't await the
call right away: one that keeps the future to await it later, or drops
it, or checks it with `coroutine::as_future()` precisely to avoid
exceptions. Such a caller expects the call to give it a future, and
isn't ready for an exception. Here is one, which stores several keys,
and is meant to report the ones it can't store, and go on:

```{.cpp include="examples/futures-and-promises/throwing.cc" snippet="store-all-bad"}
```

It doesn't work. `store("")` throws before `as_future()` has a future to
check, so the exception ends the loop, and "c", the key after the empty
one, is never stored. This can be fixed in the function, or in the
caller.

The fix in the function is to report errors in the future, and not throw
them. Like `store()`, `store_carefully()` fails on an empty key, but it
returns the error in a failed future, which it makes with
`make_exception_future()`. With it, `store_all_bad()`'s loop would work:

```{.cpp include="examples/futures-and-promises/throwing.cc" snippet="store-carefully"}
```

A function that calls code which may throw should do the same with what
that code throws: catch it, and return it with
`current_exception_as_future()` in the `catch` block. Every function that
returns a future should try to keep this. It's good advice more than a
rule, though, because it isn't easy to keep fully: almost any code that
allocates memory can throw `std::bad_alloc`, so keeping it means a `try`
around most of the function. Seastar's own asynchronous functions make
the effort, and many of them are declared `noexcept` to say so.
`semaphore::wait()`, for example, which [chapter](#ch:coordination)
introduces, puts its caller on a list of waiters, which allocates
memory, inside a `try`, and returns a `std::bad_alloc` in a failed
future.

But not every function makes this effort, and a caller that can't be
sure of the function it calls, for example because it received the
function as a parameter, can fix the problem on its side: it calls the
function through `futurize_invoke(f, args...)`, which turns an exception
that `f` throws into a failed future. `store_all_good()` calls `store()`
this way:

```{.cpp include="examples/futures-and-promises/throwing.cc" snippet="store-all-good"}
```

The program tries both callers:

```{.cpp include="examples/futures-and-promises/throwing.cc" snippet="main"}
```

and prints:

```
with store_all_bad():
stored "a"
store_all_bad() threw: empty key
with store_all_good():
stored "a"
couldn't store ""
stored "c"
```

`futurize_invoke()` also turns a result that isn't a future into an
available future, so generic code can call functions that return either. For such code,
`futurize_t<T>` names the type that `futurize_invoke()` returns, which is
`future<T>` for a type `T` that isn't a future, and `T` itself for a
future, and `is_future<T>` is true only for future types.

## Promises {index="`seastar::promise`"}

A future that isn't available yet has a [*promise*]{index="`seastar::promise`" .main} at its other end. The
future is the end that reads the result, and the promise is the end that
writes it ([Figure](#fig:future-promise)). A `promise<T>` is an object
that starts out unset. Later, `set_value()` sets it to a value of type
`T`, or `set_exception()` sets it to an exception. `get_future()`, which
may be called only once, returns the promise's future, which resolves
when the promise is set: it becomes available, with the promise's value,
or failed, with its exception. Every asynchronous function in Seastar works this way
underneath. A coroutine has a promise too, hidden in its frame, which
`co_return` sets. The network code keeps promises for the coroutines
that wait for data, and the timers keep promises for the coroutines that
sleep.

![A future and its promise. Whoever has the result, such as a timer, the
network code or a coroutine's `co_return`, writes it into the promise,
and the coroutine that waits reads it from the future. A future starts
out not available, and becomes either available, with a value, or
failed, with an exception, once.](figures/future-promise.svg){#fig:future-promise}

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

Why `co_await done.get_future()`, and not `return done.get_future()`?
Because the `co_await` is what keeps the promise and the timer alive.
Without it, `my_sleep()` wouldn't be a coroutine, and its local variables
would be destroyed when it returns: the timer would be canceled, and the
promise destroyed without being set, so the caller would get an error
right away instead of sleeping ("Broken promises", below, shows this
error). A function that returns the future without being a coroutine
must keep the promise and the timer somewhere that outlives the call.
Seastar's own `sleep()` does that: it allocates them on the heap, and
frees them when the future resolves, with a continuation
([chapter](#ch:continuations)).

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

A promise should be kept: eventually, something must call its
`set_value()` or `set_exception()`. A promise destroyed without either is [*broken*]{index="`seastar::broken_promise`" .main}, and
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

## Many waiters {index="`seastar::shared_future`"}

A future has only one reader. When many coroutines need the same result,
Seastar has `shared_future<T>`, from `<seastar/core/shared_future.hh>`,
made from an ordinary future. Any number
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
five requests together took as long as one load
([Figure](#fig:shared-future)). Exercise 2 shows a
weakness of this loader: what happens when a load fails?

![The loader while its first four requests wait. Each request waits for a
future of its own, which `get_future()` gave it, and the futures of the
requests for the same key all wait for the same `shared_future`, which
waits for a single load.](figures/shared-future.svg){#fig:shared-future}

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

### Dropping a future {index="`seastar::future`!dropping"}

What happens if a program calls an asynchronous function, and ignores the
future it returns? For example, if a coroutine calls `answer()`, from the
beginning of this chapter, like this:

```cpp
answer();
```

The compiler warns, because `future` is
declared [`[[nodiscard]]`]{index="`[[nodiscard]]`"}. GCC says:

```
warning: ignoring returned value of type 'seastar::future<int>', declared with attribute 'nodiscard' [-Wunused-result]
```

and Clang says something similar. Usually, ignoring a future is a
mistake: the code forgot to `co_await` it, and goes on before the
operation has finished. When it's deliberate, as in the echo server of
[chapter](#ch:tutorial-introduction), which served each connection in the
background, writing `(void)` before the call tells the compiler so:

```cpp
(void)answer();
```

Dropping the future doesn't stop the operation. The operation goes on
without it, and there is just nobody to learn its result.

If dropping a future doesn't stop an operation, what does? Nothing does
by force: Seastar can't interrupt a coroutine. An operation that should be
possible to cancel must be written to check, at the points where it
waits, whether someone asked it to stop, which is what the abort sources
of [chapter](#ch:time) are for. And an operation that runs in the
background must be waited for before the objects it uses are destroyed,
and before the program exits, as exercise 4 shows; that's the subject of
[chapter](#ch:object-lifetime) and [chapter](#ch:clean-shutdown).

### "Exceptional future ignored" {index="Exceptional future ignored (warning)"}

What happens, then, when an operation whose future was dropped fails?
This program drops the future of a coroutine that sleeps and then fails:

```{.cpp include="examples/futures-and-promises/ignored.cc" snippet="fail-later"}
```

```{.cpp include="examples/futures-and-promises/ignored.cc" snippet="main"}
```

It prints (with the warning shortened):

```
dropped the future
fail_later() is still running
WARN  [shard 0:main] seastar - Exceptional future ignored: std::runtime_error (nobody is listening), backtrace: 0x5851e8 ...
done
```

`fail_later()` went on after its future was dropped, and when it failed,
its exception had nowhere to go. Seastar doesn't let such an exception
disappear silently. It logs this warning, with the exception and a
backtrace, in two cases: when an exception is stored in a promise whose
future no longer exists, and when a failed future is destroyed before
anyone took its exception. Here it was the first case: the warning came
10 milliseconds after the future was dropped, when `fail_later()` failed,
and its exception was stored in the coroutine's promise, whose future was
gone.

This warning always means a bug, even when the bug is harmless: an error
happened, and nobody handled it. It has two usual causes:

- **A background operation that failed,** as above. There is no caller to
  handle its errors, so they must be handled in the operation itself. The
  echo server did that with `handle_exception()`, and
  [chapter](#ch:concurrency-within-a-shard) explains how this works, and
  the other ways to do it.
- **A failed future that the program examined, but didn't consume.**
  [Chapter](#ch:coroutines) checked futures with `coroutine::as_future()`,
  and on those that failed, called `ignore_ready_future()`. Without it,
  destroying them would have logged this warning. `ignore_ready_future()`
  says "I know this future may have failed, and I chose to ignore it". To
  handle the exception rather than ignore it, take it out of the future
  with `get_exception()`, which returns it as a `std::exception_ptr`.

The backtrace shows the code that was running when Seastar logged the
warning. Its numbers are addresses in the program, which
[chapter](#ch:debugging-and-profiling) shows how to turn into function
names and source lines. For the program above, built as usual, the
backtrace begins (with the paths shortened):

```
seastar::internal::report_failed_future(std::__exception_ptr::exception_ptr const&) at src/core/future.cc:206
seastar::internal::coroutine_traits_base<void>::promise_type::unhandled_exception() at ??:0
fail_later(_Z10fail_laterv.Frame*) (.actor.cold) at ignored.cc:0
seastar::internal::coroutine_traits_base<void>::promise_type::run_and_dispose() at ??:0
seastar::reactor::task_queue::run_tasks() at src/core/reactor.cc:2784
```

Every function is named, but only Seastar's own code, which was built
with debugging information, has line numbers; compiling the program with
`-g` adds them for its code too. When the warning is about an exception
stored in a promise whose future is gone, as here, the backtrace leads to
the code that failed: `fail_later()`, whose exception the coroutine
machinery stored in its promise. When the warning is about a failed
future destroyed before anyone took its exception, the backtrace shows
where that future was destroyed, which is often far from where the error
happened.

The exception in such a warning can be of any type, but one of
Seastar's types is worth recognizing here: `nested_exception`, which
holds two exceptions. Seastar uses it when code that runs after a failed
operation, to clean up, fails too. `finally()`, a function of the
continuation style ([chapter](#ch:continuations)), reports such double
failures this way. [Chapter](#ch:errors) describes
Seastar's other exception types.

## What futures cost {index="`seastar::future`!cost of"}

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

Why does GCC's coroutine cost more than twice as much as Clang's? At
`-O2`, GCC doesn't [inline]{index="inlining"} the coroutine's body into the function that the
caller calls, so each call to the coroutine makes a second call. At
`-O3`, or at `-O2` with four of its inlining limits raised to their `-O3`
values (the parameters `max-inline-insns-auto`, `early-inlining-insns`,
`inline-heuristics-hint-percent` and `inline-min-speedup`), GCC does
inline it, and the coroutine that doesn't wait costs 4.6 ns instead of
8.0. "Why optimization matters", below, explains inlining and its limits.

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
technique is to split a function in two: a [*fast path*]{.index .main}, an ordinary
function that handles the common case and returns an available future,
and a [*slow path*]{index="slow path" .main}, a coroutine that it calls only when it must wait.
Seastar's own `output_stream::write()` is written this way: when the data
fits in the stream's buffer, which is almost always, it copies the data
and returns `make_ready_future<>()`, and only otherwise calls a function
that waits.

(Newer compilers can sometimes avoid allocating a coroutine's frame, when
the coroutine is awaited right after the call, by putting the frame
inside the caller's frame. Seastar's `future` is marked to allow Clang to
do that, from Clang 23 on. We used Clang 22, which doesn't.)

### Why optimization matters {index="optimization"}

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
future creates an [*awaiter*]{.index .main}, which asks the future whether it's
available, which asks the future's state. Then the awaiter takes the
value out of the future, through its state and the wrapper that holds the
value, and the empty future, its state and the wrapper are destroyed.
Each of these functions is only a few instructions. Without optimization,
each is compiled separately, and called with its arguments in memory:
exercise 5 shows them in a profile. With optimization, the compiler
[*inlines*]{index="inlining" .main} them: it copies each small function into its caller, until the
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
did the same for Clang, raising its [*inline threshold*]{index="inlining!threshold" .main}, its main limit,
to 2500. Avi Kivity, who created Seastar, explained in the message of the
commit that made the change:

> With this adjustment, clang lags gcc by 3% in throughput
> (`perf_simple_query --smp 1`) compared to 20% without it.
>
> The value 2500 was derived by binary search. At 5000 compilation of
> storage_proxy never completes, at 1250 throughput is down by 10%.

`perf_simple_query` is ScyllaDB's benchmark of simple queries, here on one
shard, and `storage_proxy` is one of its larger source files. ScyllaDB's
release build still uses `-mllvm -inline-threshold=2500`, and the next
section shows what it does to Shoal.

Inlining also stops at the edge of Seastar's compiled library. Its
memory allocator, for example, and the reactor's queue of tasks, are
compiled into the library, and can't be inlined into your code, unless
the program is built with [*link-time optimization*]{.index .main} (LTO)[]{index="LTO|see{link-time optimization}"}, which optimizes
the library and the program together.
[*Profile-guided optimization*]{index="profile-guided optimization" .main}[]{index="PGO|see{profile-guided optimization}"}
(PGO) goes further: it tells the compiler which code is hot, from a
profile of the program at work. ScyllaDB's release build uses both, and
[chapter](#ch:deploying-and-tuning) explains them.

## Shoal, version 3 {index="Shoal!version 3"}

"What futures cost" suggested a fast path for code that is called very
often, and that usually has its result at hand. The request reader of
Shoal version 2 is such code. It reads every line of a request with
`read_line()`, and every string with `read_exactly()`, so a `SET` request
makes seven of these calls, and each is a coroutine. But with clients
that pipeline their requests, as the benchmarks do, a request has usually
arrived whole, so the data that each call needs is already in the
reader's buffer, and none of the coroutines waits.

Version 3[^shoal-v3] gives both functions a fast path: an ordinary
function that handles the common case and returns an available future,
and calls a slow path, a coroutine, only when it must wait. `read_line()` looks for the
line in the buffer, and if it's there, returns it in an available future;
otherwise, it calls `read_line_slow()`, the coroutine that used to be
`read_line()`:

```{.cpp include="shoal/v3/resp.cc" snippet="read-line"}
```

[^shoal-v3]: The source of this version is in `shoal/v3/`.

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

What about the advice, from "Asynchronous functions and exceptions"
earlier in this chapter, that a function that returns a future should
report its errors in that future, and not throw them? Both fast paths create an `sstring`, which may allocate memory, and could
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
requests, and with Shoal built by GCC, the best of three runs:

| Server, on one core | `SET` per second | `GET` per second |
|---|---|---|
| Shoal version 2 | 3,660,000 | 4,360,000 |
| Shoal version 3 | 4,050,000 | 4,800,000 |

Version 3 serves about 10% more `SET`s per second, and about as many
more `GET`s. `perf stat`, a Linux tool that counts what the processor
does, showed that for each `SET`, version 3 executed about 2,850
instructions in user space, against about 3,780 in version 2: a quarter
fewer. But it used only 11% fewer processor cycles. The instructions that
the fast paths saved were cheap ones, so counting instructions would have
overestimated the gain. To know how much faster code is, measure its
time.

With Clang, the result depends on its inlining limit. Built by Clang 22
the same way, at `-O2`, version 3 was hardly faster than version 2:
3,730,000 `SET`s per second against 3,650,000. Built with Clang's inline
threshold raised to 2500, as in ScyllaDB, version 3 served
3,990,000 `SET`s per second, almost as many as with GCC. ScyllaDB's
other compiler options, `-O3` and `-march=x86-64-v3` (which lets the
compiler use the instructions of newer processors), did nothing on
their own: 3,730,000 again. Whether a fast path pays off can depend
on the compiler's inlining decisions as much as on the code.

Is 10% worth the extra code? For a function on the path of every
request, yes. But this kind of optimization is worth making only where a
profile, or a measurement like this one, shows that the time goes. Most
of Shoal, and of any Seastar program, is better written as plain
coroutines.

Finally, does optimization matter as much for a whole server as it did
for the small functions of `cost.cc`? We built version 3 again with GCC, with Shoal's own code compiled at each
optimization level, always with the same `release`-mode Seastar, and
then once more with a Seastar built in `debug` mode. The best of three
runs:

| Shoal built | `SET` per second |
|---|---|
| at `-O0` (`Debug`) | 395,000 |
| at `-O1` | 3,910,000 |
| at `-O2` (`RelWithDebInfo`) | 4,050,000 |
| at `-O3` (`Release`) | 3,960,000 |
| with `debug`-mode Seastar | 26,300 |

Without optimization, Shoal served a tenth as many requests, although
only its own code, and the parts of Seastar in the headers it includes,
were compiled without optimization. Any optimization at all brought
nearly all of the speed back, and `-O3` was no faster than `-O2`. The `debug` mode was about 150 times
slower than `release`: it isn't meant to be fast, but to find bugs, as
[chapter](#ch:testing) explains.

> **Draft note, for the editors:** the `Debug` row assumes that Seastar
> pull request <https://github.com/scylladb/seastar/pull/3817> is merged.
> Until it is, a `Debug` build against a `release`-mode Seastar fails at
> run time; we measured that row with `RelWithDebInfo` and
> `-DCMAKE_CXX_FLAGS_RELWITHDEBINFO="-O0 -g"`, the same compiler options.
> Remove this note when the pull request is merged.

## Exercises

1. In `states.cc`, call `f3.get()` right after describing the futures,
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
   at `-O0`, but don't appear at all at `-O2`? Where did they go? (On a
   processor with two kinds of cores, as in many recent laptops,
   `perf report` shows a separate profile for each kind. Look at the one
   with the most samples: the other one shows little more than the
   program's start.)
