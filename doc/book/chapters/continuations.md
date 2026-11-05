# Continuations {#ch:continuations}

Before coroutines, Seastar code waited for a future by giving the future
a function to call when it was resolved: a *continuation*. The chapters
so far used coroutines instead, and so does almost all new Seastar code;
`then()`, the method that takes a continuation, appeared in only a
handful of places, such as Shoal's fast path for writing replies in
[chapter](#ch:networking). But continuations are still worth knowing,
for two reasons. A great deal of code uses them, including Seastar's
own, and anyone who reads it, or changes it, must know what it does. And
they're still the faster way to write a small function that is called
very often and usually has its result at hand, as the section "When
continuations are faster" will show.

## From callbacks to continuations {index="continuation"}

[Chapter](#ch:case-for-seastar) described the event-driven servers that
came before Seastar. Each step that had to wait, for a timer, or for
data from a client, gave the event loop a *callback*, a separate
function, to call when the wait was over. By then, the function that
started the wait had long returned, and its local variables were gone,
so whatever the next step needed was kept in a separate object, the
connection's state, which the programmer had to define, allocate, pass
to each callback, and free at the end. A simple sequence of steps became
a collection of small functions and a state machine.

A Seastar continuation is a callback too, but it's written where it's
needed, as a lambda, right after the operation that it waits for, and
its captures carry the state. This function adds one to a number, but
takes 100 milliseconds:

```{.cpp include="examples/continuations/slow-incr.cc" snippet="slow-incr"}
```

`sleep()` returns a future, and the future's
[`then()`]{index="`seastar::future::then()`" .main} method takes the
continuation, to call when the sleep is over. `then()` returns
immediately, with a future of the continuation's result, and
`slow_incr()` returns this future, long before the sleep is over. So the
continuation can't use `slow_incr()`'s parameter `i`, which no longer
exists when the continuation runs. Instead, the capture `[i]` copies `i`
into the lambda object, which `then()` keeps until it calls it. That
copy is the state that an event-driven server kept in an object of its
own, but here the compiler defines it, and `then()` frees it after the
call. Capturing `i` by reference, with `[&i]`, would be a bug: the
continuation would read the place where `i` was, after `slow_incr()`
returned and something else took that place.

A continuation can start another operation, and return its future. Then
the future that `then()` returns is a future of *that* future's result,
not a future of a future, so continuations chain, each starting when the
previous one is done:

```{.cpp include="examples/continuations/slow-incr.cc" snippet="slower-incr"}
```

The first continuation returns the future of a second sleep, so the
second continuation runs only after both sleeps, 150 milliseconds after
the call. Functions that return futures chain the same way, each
continuation receiving the result of the previous one:

```{.cpp include="examples/continuations/slow-incr.cc" snippet="add-three"}
```

The program calls the three functions one after another, also with
continuations:

```{.cpp include="examples/continuations/slow-incr.cc" snippet="main"}
```

and prints:

```
100 ms: slow_incr(1) = 2
250 ms: slower_incr(1) = 2
551 ms: add_three(1) = 4
```

The steps of each operation stay together, in order, in the source, and
each continuation gets what it needs from its captures and its argument.
Errors travel along the chain too: a failed future skips the
continuations after it, to the one place that handles errors, as the
section "Errors" will show, instead of every callback checking for
errors of its own. That was a big step forward from callbacks.

## Objects that continuations use {index="continuation!object lifetime"}

Copying an `int` into a capture costs nothing, but copying a large
object costs time, and some objects, such as a `std::unique_ptr`, can't
be copied at all. A capture with an initializer can move the object into
the continuation instead, which then owns it, and destroys it when it's
done with it:

```{.cpp include="examples/continuations/ownership.cc" snippet="move"}
```

`slow_sum()` owns `v`, its parameter, but returns before the
continuation runs, so it moves `v` into the continuation, without copying
the vector's elements. A continuation can only read its captures, unless
it's declared `mutable`: a lambda's `operator()` is `const` by default,
as the C++ corner "lambdas are objects", in
[chapter](#ch:object-lifetime), explained. A continuation
that moves a captured object on, or changes it, is written
`[v = std::move(v)] () mutable { ... }`.

Moving works for one continuation. When several continuations in a chain
use the same object, each must move it on to the next, by returning it,
or capturing it again, which soon becomes tiresome. The other way is to
keep the object somewhere that outlives the whole chain, and let the
continuations capture references to it. Seastar's convention, from
[chapter](#ch:object-lifetime), says where: a function that takes a
reference may use the object until its future is resolved, and its
caller must keep the object alive until then. But the caller has the
same problem, since it too returns before the chain ends.
[`do_with()`]{index="`seastar::do_with()`" .main}, from
`<seastar/core/do_with.hh>`, solves it: it moves its arguments into
memory that it allocates, calls its function with references to them, and
destroys them only when the future that the function returned is
resolved:

```{.cpp include="examples/continuations/ownership.cc" snippet="by-reference"}
```

This returns `2`. A single missing `&` breaks it, with no warning from
the compiler. If the function given to `do_with()` took
`std::vector<int> v`, without the `&`, it would get a copy, which would
die when the function returned, long before `slow_append_twice()`'s
continuations used it. And if `slow_append_twice()` itself took its
parameter by value, its continuations would capture a reference to that
parameter, which dies as soon as `slow_append_twice()` returns. Seastar's
old tutorial recommended always writing the parameters of the function
given to `do_with()` as `auto&`, so that the `&` is harder to forget.

## Loops {index="continuation!loops"}

Continuations become harder still when the code loops. Here is [chapter](#ch:coroutines)'s `count_bytes()`, which
reads from a client until it closes its side of the connection, written
with continuations:

```{.cpp include="examples/continuations/count-bytes-then.cc" snippet="count-bytes"}
```

A continuation is called once, but the loop must read again and again,
and a loop of continuations needs a function that starts each turn when
the previous one ends: [`repeat()`]{index="`seastar::repeat()`" .main},
from `<seastar/core/loop.hh>`, calls its function again until the
function's future says `stop_iteration::yes`. And every turn of the loop
adds to the same `n`, so `n` can't be a copy in a capture, of which each
continuation would have its own, nor a local variable of
`count_bytes()`, which returns long before the loop ends, so
`do_with()` keeps it.

`report()` chains three steps, each starting when the previous one is
done:

```{.cpp include="examples/continuations/count-bytes-then.cc" snippet="report"}
```

Again `do_with()` keeps the two streams alive, and the continuations
capture references to them, which stay valid because `do_with()` keeps
the objects until the whole chain is done. Capturing a local variable of
`report()` by reference would have been the bug of `[&i]` above:
`report()` returns immediately, with the chain's future, and its local
variables die then, long before the continuations run. That's the most
common bug of continuation code, and the reason for every `do_with()`.

The server's loop, which never ends, uses
[`keep_doing()`]{index="`seastar::keep_doing()`"}, which calls its
function again and again, as long as its future succeeds:

```{.cpp include="examples/continuations/count-bytes-then.cc" snippet="serve"}
```

The program behaves exactly as [chapter](#ch:coroutines)'s, and
comparing the two shows what coroutines changed. Here is the coroutine
`count_bytes()` again:

```{.cpp include="examples/coroutines/count-bytes.cc" snippet="count-bytes"}
```

A coroutine's local variables live in its frame, which stays alive until
the coroutine ends, however many times it waits, so `n`, like
`report()`'s two streams, is an ordinary local variable, and needs no
`do_with()`. And a coroutine waits inside an ordinary `while` loop, so
it needs no `repeat()` or `keep_doing()`.

The rest of Seastar's loops, which old code uses, are variations on the
same idea.
[`repeat_until_value()`]{index="`seastar::repeat_until_value()`"} stops
when its function's future holds a value, in a `std::optional`.
[`do_until()`]{index="`seastar::do_until()`"} takes a condition and a
function. And [`do_for_each()`]{index="`seastar::do_for_each()`"} calls
a function for each element of a range, one after another, waiting for
each. New code writes a loop in a coroutine instead. One warning about
all of them: a loop function may move the function that it's given into
memory that it allocates, when it first has to wait, so a continuation must
never refer to that function's captures by reference, as exercise 5
discovers.

## Seastar and C++: a short history {index="Seastar!history"}

Continuation code looks the way it does because of when it was written.
Each feature of C++ that Seastar uses arrived to fix a pain, and old
Seastar code shows the workarounds for pains that later versions of the
language removed. The dates of Seastar's side of this story come from its
history in git.

- **C++11** gave lambdas, move semantics and `std::unique_ptr`, and
  variadic templates: the raw material of futures and continuations.
  But a lambda couldn't capture an object by moving it, only by copying
  it, or by reference.
- **C++14** gave captures with an initializer, such as
  `[buf = std::move(buf)]`, without which a continuation can't own an
  object that can only move. Seastar's first commit, by Avi Kivity, on
  August 10, 2014, was already compiled with `-std=gnu++1y`, the draft of
  C++14.
- **C++17**, Seastar's default from December 2017, guaranteed the order
  of evaluation in chained calls, which made a pattern such as
  `p->get().then([p = std::move(p)] { ... })` safe
  ([chapter](#ch:object-lifetime)), and added `std::optional`,
  `std::variant` and `std::string_view`.
- **Coroutines** came to Seastar on January 8, 2019, in commits by Paweł
  Dziepak, built on the Coroutines Technical Specification, a year before
  C++20 standardized them.
- **The Lambda Coroutine Fiasco** of [chapter](#ch:object-lifetime) was
  documented, and partly mitigated, in September 2022; C++23's
  `this auto` cures it.
- **The standards that Seastar supports** were C++20 and C++23 from
  January 2024, when C++17 was dropped, and are C++23 and C++26 since
  June 2026.

So `do_with()` and `repeat()` were workarounds for what the language
lacked: a coroutine keeps its variables in its frame, and loops with
`while`. Knowing which idioms are workarounds makes old code easier
to read, and to modernize.

## Errors {index="continuation!errors"}

When a future fails, `then()` doesn't call its continuation, but returns
a future that fails with the same exception, so a failure skips the rest
of the chain, until something handles it. A continuation may also throw:
`then()` catches the exception, and returns it as a failed future. So a
chain of continuations behaves like a sequence of statements, in which
an exception skips the statements after it:

```cpp
return step1().then([] {
    return step2();       // fails
}).then([] {
    return step3();       // skipped
});
```

[`handle_exception()`]{index="`seastar::future::handle_exception()`"}
calls its function with the exception, as a `std::exception_ptr`, and
[`handle_exception_type()`]{index="`seastar::future::handle_exception_type()`"}
only with exceptions of the type that its function takes, letting others
through. [`finally()`]{index="`seastar::future::finally()`" .main} runs its
function either way, as the cleanup after a `try` block does. This
function parses a number, after waiting a millisecond:

```{.cpp include="examples/continuations/errors-then.cc" snippet="parse"}
```

and this one doubles it, and prints it, or the error, with
`handle_exception_type()` and `finally()`:

```{.cpp include="examples/continuations/errors-then.cc" snippet="chain"}
```

Called with "21", with "abc" and with an empty string:

```{.cpp include="examples/continuations/errors-then.cc" snippet="calls"}
```

it prints:

```
"21": 42
"21": done
"abc": not a number (stoi)
"abc": done
show("") threw: empty
futurize_invoke: a failed future: empty
```

The empty string shows the pitfall that [chapter](#ch:futures-and-promises)
warned of. `parse()` doesn't return a failed future; it throws, before it
returns anything, so the chain that `show()` built on its future never
existed: neither `handle_exception_type()` nor even `finally()` ran, and
the exception went to `show()`'s caller, which a coroutine's caller would
never see. That's why an asynchronous function should return failures as
failed futures, with `make_exception_future()`, and why code that calls a
function that might throw wraps the call in `futurize_invoke()`, which
turns the exception into a failed future. In a coroutine, this pitfall
doesn't exist: an exception thrown anywhere in a coroutine becomes its
failed future.

[`then_wrapped()`]{index="`seastar::future::then_wrapped()`" .main}, the
most general of these methods, calls its function with the future
itself, failed or not, as the last call above does; its function can ask
`failed()`, or call `get()` and catch what it throws.

## Waiting for several futures {index="`seastar::when_all_succeed()`"}

[Chapter](#ch:concurrency-within-a-shard) waited for several operations
together with `when_all_succeed()`, whose future holds a tuple of their
values, which a coroutine gets with `co_await`. A continuation gets the
tuple as its argument, or with
[`then_unpack()`]{index="`seastar::future::then_unpack()`"}, each value
as an argument of its own. This waits for two calls of
`slow_number(n, delay)`, which returns `n` after `delay`, as
`slow_incr()` returned `i + 1`:

```{.cpp include="examples/continuations/when-all.cc" snippet="unpack"}
```

`when_all()`, which never fails, gives a tuple of futures, each available
and either succeeded or failed:

```{.cpp include="examples/continuations/when-all.cc" snippet="tuple"}
```

This prints `when_all_succeed: 1 + 2 = 3` and `when_all: 3, and a
failure`. The failed future must be dealt with, by `get()` or by
`ignore_ready_future()`, or its destructor complains that its failure was
ignored ([chapter](#ch:futures-and-promises)).

## When continuations are faster {index="continuation!cost"}

In Seastar's optimized builds, `then()` on a future that is already
available doesn't schedule anything: it calls the continuation immediately,
and returns its result as a future, with no task and no allocation. A
coroutine allocates its frame on every call, even if every future it
awaits is available. So a small function that usually finds its result at
hand, such as one that only transforms the result of another, is cheaper
as a continuation. This program measures it:

```{.cpp include="examples/continuations/cost-then.cc" snippet="functions"}
```

Each function was called a hundred million times, from a coroutine that
awaits each future, as in [chapter](#ch:futures-and-promises)'s
measurement, on one shard; the best of three runs:

| Function | GCC 16 | Clang 22 |
|---|---|---|
| `ready()` | 2.0 ns | 1.6 ns |
| Doubled with `then()` | 2.7 ns | 2.7 ns |
| Doubled in a coroutine | 9.9 ns | 9.6 ns |

The continuation adds about a nanosecond to the available future; the
coroutine adds about eight, for its frame. When the future isn't
available, the advantage is gone: a continuation that must wait needs a
task, allocated like a coroutine's frame, and exercise 4 finds the two
equally fast then. For most code, the difference is too small to
matter: a Shoal request takes hundreds of nanoseconds. But in a function
called many times for each request, it adds up, and Seastar's own hot
paths, such as its streams, are written as plain functions that return
available futures, falling back to a coroutine or a continuation only
when they must wait.

Three cautions come with the fast path. It doesn't check for
preemption: a chain of continuations that all find their futures
available runs to the end in one task, however long it is, and can stall
the shard ([chapter](#ch:stalls)). Seastar's loop functions, such as
`repeat()`, check for preemption between turns, but a chain that a
program builds itself, for example with recursion, doesn't. Worse, such
a recursion runs on the stack: a function that calls itself in the
continuation of an available future, as exercise 3 does, calls itself
immediately, and a million levels of it overflow the stack and crash the
program. And in debug builds, `then()` schedules every continuation as a
task, even on an available future, so that tests see continuations run
later than they would in production, and code that wrongly depends on
running immediately breaks there first. The other side of this is that a
recursion like exercise 3's works in a debug build, which turns it into
a chain of tasks, and crashes only in an optimized build.

## Translating continuations into coroutines {index="continuation!translating to coroutines"}

ScyllaDB has been translating its continuations into coroutines for
years: in the year to November 2026, the number of `.then(` calls in its
code fell from 1,506 to 1,152, while its `co_await`s grew from 7,126 to
9,771. The translation is mostly mechanical, and the two versions of
`count_bytes()` and `report()` above, and in [chapter](#ch:coroutines),
show its rules:

- A chain `f().then([] (T x) { ... })` becomes `T x = co_await f();`
  followed by the continuation's body, and a continuation that returns a
  future becomes another `co_await`.
- `do_with(x, [] (T& x) { ... })` becomes a local variable: the
  coroutine's frame keeps it alive for as long as the coroutine runs.
- `repeat()`, `keep_doing()` and the other loops become `while` and `for`
  loops, with `co_await` inside.
- `handle_exception()` becomes `try` and `catch`, `then_wrapped()`
  becomes `co_await coroutine::as_future(...)`, which gives the future
  itself, and `finally()` becomes cleanup that runs on both paths, as
  [chapter](#ch:clean-shutdown) showed.
- Captures by value become local variables, or parameters, which a
  coroutine keeps in its frame; captures by reference of objects that
  `do_with()` kept alive become references to those local variables.

Two things need thought rather than rules. A function that returned a
future without being a coroutine, and threw before returning it, now
returns the exception as a failed future, so a caller that caught it
must check the future instead. And a function on a hot path, which was a
continuation for speed, may be better left alone, or split into a fast
path that returns an available future and a coroutine for the slow path.

## Exercises

1. Translate `when-all.cc`'s `add_two()` and `one_fails()` into
   coroutines. How do you get each future of `when_all()`'s tuple?

2. Fix `parse()` so that it never throws, and returns every failure as a
   failed future. Check that `show("")` now prints `done`, without
   `futurize_invoke()`.

3. Write a function that counts down from a number with continuations,
   by calling itself in the continuation of a future that is already
   available, `make_ready_future<>()`, until it reaches zero. Call it with
   a hundred thousand, and with ten million. What happens, and why? Then
   write it with `repeat()`, and call it with ten million, while a timer
   that should fire every millisecond measures how late it gets.

4. Measure what the functions of `cost-then.cc` cost when `ready()`
   waits, with `co_await yield()`, before it returns. Is the
   continuation still cheaper?

5. Write `count_bytes()` with
   [`repeat_until_value()`]{index="`seastar::repeat_until_value()`"}
   instead of `repeat()`, so that it needs no `do_with()`. Where does the
   count live from one turn of the loop to the next?
