# Errors {#ch:errors}

In Seastar, as in the rest of C++, an error is an exception. What Seastar
adds is where the exception goes: a coroutine that fails stores its
exception in its future, and the coroutine that awaits the future gets it
back, as [chapter](#ch:coroutines) showed. That chapter also showed the
two ways to report an error from a coroutine: throwing it, which costs
about 0.8 microseconds to throw and catch, and returning it in a failed
future, with `coroutine::return_exception()`, which a caller can check
with `coroutine::as_future()` without throwing anything, for about 50
nanoseconds. And [chapter](#ch:futures-and-promises) showed why a
function that returns a future should report its errors in the future,
and not throw them.

This chapter is about the errors themselves. It starts with the
exceptions that Seastar's own functions report, and how to tell them
apart, and how to print them. Then it shows what exceptions do to a
server that is overloaded, which is when errors are most frequent, and
how large Seastar applications avoid exceptions where errors are
expected: by not throwing them, and by not creating them at all. It ends
with two kinds of errors that are rare, and worth extra information:
exceptions that carry a backtrace, and internal errors, which mean that
the program has a bug.

## Seastar's exceptions {index="exceptions!Seastar's"}

When one of Seastar's waits can't end the way its caller hoped, its
future fails with an exception of a type that says why. A semaphore whose
wait timed out ([chapter](#ch:coordination)) fails it with
`semaphore_timed_out`, and a `sleep_abortable()` whose abort source was
triggered ([chapter](#ch:time)), with `sleep_aborted`. These are the
types you will meet most often:

| What happened | Exception | Reported by |
|---|---|---|
| A timeout | [`timed_out_error`]{index="`seastar::timed_out_error`"} | `with_timeout()`, a `shared_future`'s wait |
| | [`semaphore_timed_out`]{index="`seastar::semaphore_timed_out`"} | a semaphore's wait |
| | [`condition_variable_timed_out`]{index="`seastar::condition_variable_timed_out`"} | a condition variable's wait |
| An abort | [`abort_requested_exception`]{index="`seastar::abort_requested_exception`"} | `abort_source::check()` |
| | [`sleep_aborted`]{index="`seastar::sleep_aborted`"} | `sleep_abortable()` |
| | [`semaphore_aborted`]{index="`seastar::semaphore_aborted`"} | a semaphore's wait |
| A broken object | [`broken_promise`]{index="`seastar::broken_promise`"} | a promise destroyed unkept |
| | [`broken_semaphore`]{index="`seastar::broken_semaphore`"}, [`broken_condition_variable`]{index="`seastar::broken_condition_variable`"} | waits on a broken one |
| A closed object | [`gate_closed_exception`]{index="`seastar::gate_closed_exception`"} | entering a closed gate |
| Two failures | [`nested_exception`]{index="`seastar::nested_exception`"} | a cleanup that failed too |

The exception types form families. `semaphore_timed_out` is a kind of
`timed_out_error`, so code that catches a `timed_out_error` catches
every timeout of a semaphore, a `shared_future` or `with_timeout()`, and
`sleep_aborted` and `semaphore_aborted` are kinds of
`abort_requested_exception`, so catching that catches every abort of
Seastar's waits. (Each named semaphore, and each named gate, of
[chapter](#ch:coordination) has a type of its own too, a kind of the
unnamed one, whose message names it.) This function tells two families
apart:

```{.cpp include="examples/errors/kinds.cc" snippet="kind"}
```

It rethrows the exception, because, as [chapter](#ch:coroutines)'s C++
corner on `std::exception_ptr` explained, rethrowing is the only way
before C++26 to ask what type of exception a `std::exception_ptr`
holds. This function waits for a future, and if it failed, says what
kind of failure it was:

```{.cpp include="examples/errors/kinds.cc" snippet="report"}
```

And this program makes some of Seastar's waits fail:

```{.cpp include="examples/errors/kinds.cc" snippet="main"}
```

(`broken()` is the function of [chapter](#ch:futures-and-promises) that
returns the future of a promise and destroys the promise.) It prints:

```
semaphore wait: a timeout: seastar::semaphore_timed_out (Semaphore timedout)
condition wait: something else: seastar::condition_variable_timed_out (Condition variable timed out)
sleep: an abort: seastar::sleep_aborted (Sleep is aborted)
semaphore wait: an abort: seastar::semaphore_aborted (Semaphore aborted)
gate: something else: seastar::gate_closed_exception (gate closed)
promise: something else: seastar::broken_promise (broken promise)
```

One line is a surprise: a condition variable's timeout is not a
`timed_out_error`, so the catch for timeouts misses it. The families are
a convenience, not a rule that every Seastar type follows, so before
relying on one, check the exception's base class in its header. All of
these exceptions are kinds of `std::exception`, so a catch of
`std::exception` gets them all, and their `what()` gives the message.

A [`nested_exception`]{index="`seastar::nested_exception`" .main} holds two exceptions. Seastar reports
one when code that runs after an operation fails, to clean up, fails too.
Then neither exception may be lost: the operation's own failure, and the
cleanup's. `finally()`, a function of the continuation style
([chapter](#ch:continuations)), reports such double failures this way.

## Printing an exception {index="exceptions!printing"}

To print an exception, or log it, wrap the `std::exception_ptr` in
[`seastar::formattable()`]{index="`seastar::formattable()`" .main}, as `report()` above did. It prints the type of
the exception, followed by its message, and for a `nested_exception`,
both of the exceptions inside it:

```{.cpp include="examples/errors/kinds.cc" snippet="nested"}
```

prints:

```
seastar::nested_exception: std::runtime_error (disk error) (while cleaning up after std::runtime_error (network error))
```

The type matters: a message alone, such as "gate closed", doesn't say
which code reported it. `formattable()` also prints the error code of a
`std::system_error`, which is how Seastar reports a failed system call.
It works with `fmt::format()` and with Seastar's logger:
`applog.warn("connection failed: {}", formattable(e))`, as the servers of
[chapter](#ch:tutorial-introduction) log a connection's failure.

Why wrap the `std::exception_ptr` at all, instead of printing it? {fmt}
can print only types for which it has a *formatter*, a specialization of
its template `fmt::formatter`, and its releases so far have none for
`std::exception_ptr`. (One was added to {fmt}'s development version in
2026, by <https://github.com/fmtlib/fmt/pull/4819>, so a future release
will print a `std::exception_ptr` by itself.
Even then, `formattable()` remains the way to print one in Seastar code:
it works with every version of {fmt}, and it prints both of the
exceptions of a `nested_exception`.) Seastar could define a formatter for
`std::exception_ptr` itself, but a library shouldn't
specialize another library's template for a type that it doesn't own:
if another library, or {fmt} itself, ever defines one too, the two
definitions clash. So `formattable()` wraps the pointer in a small
type of Seastar's own, which only Seastar formats. It accepts only a
`std::exception_ptr`. An exception object that you have already caught,
such as the `e` of `catch (std::exception& e)`, {fmt} prints by itself,
with `<fmt/std.h>`: `"{}"` prints its message, and `"{:t}"` its type too,
as in "std::runtime_error: disk error".

> **Draft note, for the editors:** when a {fmt} release includes
> fmtlib/fmt pull request 4819 (12.2.1 doesn't), say which version, and
> check the paragraph above against it. Also check whether Seastar's
> pull request <https://github.com/scylladb/seastar/pull/3832> (keeping
> Seastar's deprecated formatter until then) and
> <https://github.com/scylladb/seastar/pull/3586> (letting {fmt} see
> inside a `nested_exception`) were merged, and whether the claim about
> `nested_exception` still holds.

## Exceptions under overload {index="exceptions!under overload"}

[Chapter](#ch:coroutines) measured what an exception costs: throwing it
and catching it took about 0.8 microseconds, and returning it in a failed
future and checking it with `coroutine::as_future()`, about 50
nanoseconds. Less than a microsecond is little for an error that happens
once in a while. But some errors aren't rare, and they are most frequent exactly
when the CPU can least afford them: when a server is overloaded, requests
wait longer, and more of them time out.

This program shows what happens then. Its server admits one request at a
time, through a semaphore, as Shoal admits requests by memory in
[chapter](#ch:coordination), and serving a request takes 10 microseconds
of CPU. Each client waits at most a millisecond to be admitted, and when
it times out, it tries again at once, as many real clients do. Here is
the client, written with an exception for the timeout:

```{.cpp include="examples/errors/overload.cc" snippet="with-exceptions"}
```

and written without it, with `as_future()`:

```{.cpp include="examples/errors/overload.cc" snippet="without-exceptions"}
```

The program runs each kind of client for two seconds, with more and more
clients, on one core. In three runs each:

| Clients | Served per second, with exceptions | Served per second, without |
|---|---|---|
| 50 | 98,300–98,400 | 98,300 |
| 100 | 89,500 | 90,100–90,200 |
| 200 | 81,200 | 87,700–87,900 |
| 500 | 41,100–42,500 | 80,600–81,300 |
| 1,000 | 1,240–1,380 | 71,100–71,600 |
| 2,000 | 416–464 | 54,700–55,500 |

With 50 clients, no request waits long enough to time out, and the
server serves about 98,000 requests a second, nearly the 100,000 that
10 microseconds each allow. From 100 clients on, the queue is longer than
a millisecond of work, and clients start to time out. Without
exceptions, the server slows down gradually: with 2,000 clients, 1.6
million timeouts a second still leave it more than half of its time to
serve requests. With exceptions, it collapses: with 500 clients it serves
half as many, and with 1,000 or more, almost nothing. It spends all its
time on failures, about 570,000 timeouts a second, at about 1.7
microseconds each, which include throwing and catching the exception.

This is what large Seastar applications call an [*exception storm*]{.index .main}.
The number of timeouts doesn't depend on how fast the server works: each
waiting client times out once a millisecond. So when the exceptions take
the CPU, fewer requests are served, the queue grows, and even more
clients time out. A server that should slow down gracefully under
overload stops doing useful work at all.

ScyllaDB met this in 2022, when timeouts on an overloaded server were
reported with exceptions. Piotr Dulikowski explained, in the message of
the change that began the fix:

> Analyzing the value and type of an exception held by
> `std::exception_ptr`'s cannot be done without rethrowing the
> exception, and that can be very costly even if the exception is
> immediately caught. Because of that, exceptions are not a good fit for
> reporting failures which happen frequently during overload, especially
> if the CPU is the bottleneck.

Since then, ScyllaDB has removed exceptions from its paths where errors
are frequent, one path at a time, and the work continues. Its code now
uses `coroutine::return_exception()` about 200 times, and
`coroutine::as_future()` about 120 times. Redpanda chose early to report
most errors as values: it has 51 enumerations of error codes, and its
result types, which hold either a value or an error code, appear more
than 1,300 times in its code.

### Errors as values {index="`std::expected`"}

`as_future()` avoids throwing an exception that Seastar created, such as
a semaphore's timeout. But creating the exception costs too: an
allocation, and the exception's construction, about 35 nanoseconds even
for a small one. For errors of your own code that are part of its normal
work, such as a key that isn't found, you can avoid creating an
exception at all, by returning the error as a value. C++23's
[`std::expected<T, E>`]{index="`std::expected`" .main} holds either a value of type `T` or an error of
type `E`, and it fits in a future:

```{.cpp include="examples/errors/expected.cc" snippet="get-number"}
```

`std::unexpected(e)` makes an `std::expected` that holds the error `e`.
The caller checks which it got, as it would check a pointer:

```{.cpp include="examples/errors/expected.cc" snippet="main"}
```

which prints:

```
x is 42
y isn't a number
z wasn't found
```

A failure reported this way costs no more than a success. The price is
in the code: every caller must check the result, and pass the error on
itself if it can't handle it, where an exception would have passed
through on its own. That is why both ScyllaDB and Redpanda use values
for the errors that are frequent, and keep exceptions for the rest.

## Exceptions with backtraces {index="exceptions!with a backtrace"}

An exception says what went wrong, but not where. For an error that is
rare, and hard to understand from its message alone, Seastar can attach
a backtrace to the exception, with the functions of
`<seastar/util/backtrace.hh>`. [`make_backtraced_exception_ptr<E>(args...)`]{index="`seastar::make_backtraced_exception_ptr()`" .main}
makes a `std::exception_ptr` holding an exception of a type derived from
`E`, so it is caught as an `E`, whose `what()` is the message given in
`args`, followed by " Backtrace: " and the addresses of the functions
that were running when it was made.
[`throw_with_backtrace<E>(args...)`]{index="`seastar::throw_with_backtrace()`" .main} makes the same exception and
throws it. [Chapter](#ch:debugging-and-profiling) shows how to turn the
addresses into function names and source lines.

A backtrace is expensive to collect: on our laptop, creating an
exception with a backtrace took about 11 microseconds, against 35
nanoseconds without one. So use backtraces only for errors that are
rare, and never for the frequent errors of the previous section.

## Internal errors {index="internal errors"}

Some errors mean that the program itself is wrong: a check of something
that can't happen, unless the code has a bug. C++ programs usually check
such things with `assert()`, which stops the program when the check
fails. Stopping a server for every bug is often too much: if one request
found a bug, the server could fail that request, and go on serving the
others. Seastar's [`on_internal_error()`]{index="`seastar::on_internal_error()`" .main}, from
`<seastar/core/on_internal_error.hh>`, does that:

```{.cpp include="examples/errors/internal.cc" snippet="withdraw"}
```

`on_internal_error()` logs the message as an error, with a backtrace, and
counts it in a metric, `internal_errors`, so that monitoring notices that
a bug was hit. Then it throws a `std::runtime_error`, with the same
message and a backtrace, which fails the operation that found the bug,
and only it:

```{.cpp include="examples/errors/internal.cc" snippet="main"}
```

The program logs

```
ERROR [shard 0:main] app - negative balance -50 after withdrawing 150, at: 0x5853a8 0x585c66 ...
```

and prints "the withdrawal failed, and the program goes on".

Going on isn't always what you want. In tests, a bug should stop the
test where it was found, so that it can't be missed, or hidden by a
failure that it causes later. [`set_abort_on_internal_error(true)`]{index="`seastar::set_abort_on_internal_error()`" .main} makes
every `on_internal_error()` abort the program instead of throwing.
ScyllaDB, which calls `on_internal_error()` more than 800 times, has a
configuration option, `abort_on_internal_error`, which calls this
function, and its test runner turns it on.

Two variants complete the family. [`on_internal_error_noexcept()`]{index="`seastar::on_internal_error_noexcept()`"} logs
the error, and aborts if aborting was set, but doesn't throw, so it can
be called where an exception can't escape, such as a destructor.
[`on_fatal_internal_error()`]{index="`seastar::on_fatal_internal_error()`"} logs the error and always aborts, for a bug
after which the program can't safely go on, such as corrupted data.

### Assertions {index="assertions"}

C++ programs usually check for bugs with the standard
[`assert()`]{index="`assert()`"}. But as [chapter](#ch:getting-started) explained, `assert()` is
compiled away in builds that define `NDEBUG`, which optimized builds
usually do, so it checks nothing in production, where a bug matters most.
So Seastar's own code checks its assumptions with
[`SEASTAR_ASSERT()`]{index="`SEASTAR_ASSERT()`" .main}, from `<seastar/util/assert.hh>`, which is checked in
every build mode, and uses `SEASTAR_DEBUG_ASSERT()` only for checks that
are too slow for optimized builds. Seastar replaced its `assert()`s with
`SEASTAR_ASSERT()` in 2025, and ScyllaDB had done the same in 2024, with
a `SCYLLA_ASSERT()` of its own, which it now uses more than 1,300 times.

An assertion that is always checked has the opposite problem. When it
fails, it aborts the program, so in a server, one request that found a
bug becomes an outage for every client. And its message, and the
backtrace that Seastar prints when the program aborts, go to the
standard error output, not to the program's log.

The alternative is the `on_internal_error()` of the previous section,
which logs the failure with a backtrace, and fails only the operation
that found the bug, except in tests, which make it abort. Its only
drawback is that it's longer to write than an assertion: an `if`, and a
message. ScyllaDB, which has had an open issue since 2021 to "exorcise
assertions from Scylla", added in 2026 a macro, `throwing_assert()`,
which calls `on_internal_error()` but is written like `assert()`, so that
its many assertions can be converted one line at a time. Seastar has no
such macro, but it takes only a few lines to write:

```{.cpp include="examples/errors/internal.cc" snippet="check"}
```

It's used like `assert()`:

```{.cpp include="examples/errors/internal.cc" snippet="deposit"}
```

A call with a negative `n` logs "check failed: n > 0", with a backtrace,
and throws. `CHECK()` can only report the text of the condition, so when
the values involved would help to find the bug, as in `withdraw()`, call
`on_internal_error()` with a message that includes them.

## Exercises

1. In `kinds.cc`, replace `broken()` by its body, inline:
   `co_await report("promise", promise<>().get_future());`. What happens
   when you run the program, and why? (Hint: when is a temporary object
   destroyed?) Can you stop the program with Ctrl-C?
   [Chapter](#ch:clean-shutdown) explains what you see.

2. In `overload.cc`, make the clients wait up to 10 milliseconds instead
   of 1, and run it with ten times as many clients. Before you run it,
   predict the results. What does a longer timeout change, and what
   doesn't it?

3. If your compiler supports C++26, rewrite `kind()` in `kinds.cc` with
   `std::exception_ptr_cast` ([chapter](#ch:coroutines)), without
   rethrowing. Does it still report a `semaphore_timed_out` as a timeout?

4. In `internal.cc`, call `set_abort_on_internal_error(true)` before the
   withdrawal. What does the program print now, and what is its exit
   status? Where would you want this behavior, and where wouldn't you?

5. In `internal.cc`, print the caught exception's `what()`. Build the
   program with `-g`, and decode the backtrace at the end of the message
   with `seastar-addr2line`, from Seastar's `scripts` directory: run it as
   `seastar-addr2line -e PROGRAM`, and give it the addresses on its
   standard input. ([Chapter](#ch:debugging-and-profiling) explains
   decoding backtraces in full.) Which function does it point to?
