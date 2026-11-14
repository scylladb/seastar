# Errors {#sol:errors}

## Exercise 1

Here is `kinds.cc` with `broken()` replaced by its body, inline:

```{.cpp include="examples/solutions/errors-ex1.cc"}
```

It printed its first five lines, the ones before the promise, and then
hung, doing nothing. `kill -INT`, the signal that Ctrl-C sends, didn't
stop it, nor did `kill`; only `kill -KILL` did.

`broken()` worked because its promise was a local variable, destroyed
when `broken()` returned, before anyone waited for the future: a promise
destroyed without a value breaks its future, which then fails with
`broken_promise`. Inline, `promise<>()` is a temporary, and a temporary
is destroyed at the end of the *full expression* that created it, which
here is the whole statement `co_await report("promise",
promise<>().get_future());`, including the `co_await`. So the promise
lives until `report()` is done, `report()` waits for the promise's
future, and nothing will ever fulfill the promise, or destroy it. The
coroutine waits forever, and the program with it.

Ctrl-C doesn't help, for the reason that [chapter](#ch:clean-shutdown)
explains: `app_template` installs handlers for `SIGINT` and `SIGTERM`,
which start an orderly shutdown, and the shutdown first waits for the
function given to `run()` to finish, which it never does. A program
whose main function can hang must handle the signals itself, and make
whatever it waits for stop.

## Exercise 2

`overload.cc`, with clients that wait up to 10 milliseconds, and ten
times as many of them:

```{.cpp include="examples/solutions/errors-ex2.cc"}
```

The prediction: the limit is the rate of timeouts. Requests are served
one at a time, each in 10 microseconds, so with *n* clients waiting, a
client waits about *n* × 10 microseconds for its turn. With a timeout
of 1 millisecond, the waits start timing out at about 100 clients; with
10 milliseconds, at about 1,000. Beyond that, a client that times out
retries at once, so the clients time out about *n* divided by the
timeout times a second: ten times the timeout, and ten times the
clients, give the same rate of timeouts. So the results should be the
chapter's, at ten times as many clients.

They are. In three runs, pinned to one core, against the chapter's
table for a timeout of 1 millisecond:

| Clients (10 ms) | Served a second, with / without exceptions | Clients (1 ms) | Served a second, with / without exceptions |
|---|---|---|---|
| 500 | 98,764–98,772 / 98,736–98,771 | 50 | 98,362–98,421 / 98,295–98,350 |
| 1,000 | 89,803–89,877 / 90,581–90,652 | 100 | 89,500–89,542 / 90,095–90,180 |
| 2,000 | 79,703–80,204 / 88,077–88,224 | 200 | 81,190–81,213 / 87,722–87,914 |
| 10,000 | 796–886 / 70,575–70,907 | 1,000 | 1,238–1,377 / 71,075–71,608 |
| 20,000 | 440–449 / 52,555–52,964 | 2,000 | 416–464 / 54,662–55,500 |

(and 5,000 clients, between the chapter's 500 and 1,000, served
38,424–39,455 a second with exceptions, and 81,450–81,827 without.) With
exceptions, the timeouts again leveled off at about 600,000 a second,
which is all the shard can throw and catch.

A longer timeout moves the collapse further out, to ten times as many
clients, but doesn't prevent it: it changes how many clients it takes to
time out at a given rate, not what a timeout costs, or how many requests
the server can serve. And it has a price of its own: each client that
can't be served waits ten times as long before it learns so. The real
cures are the chapter's: make failures cheap, without exceptions, and
don't let clients retry at once, without limit.

## Exercise 3

`kind()`, with `std::exception_ptr_cast`:

```{.cpp include="examples/solutions/errors-ex3.cc"}
```

Built with GCC 16 and `-std=c++26`, which has `std::exception_ptr_cast`
(`__cpp_lib_exception_ptr_cast` is 202506), it printed exactly the
lines that the chapter's `kinds.cc` does, with `semaphore wait: a
timeout: seastar::semaphore_timed_out (Semaphore timedout)` first. So
yes: `std::exception_ptr_cast<timed_out_error>()` returns a pointer for a
`semaphore_timed_out`, because, like a `catch` clause, it matches an
exception of the given class or of any class derived from it. It differs
from the `catch` version only in how: it looks at the exception where it
is, without throwing it again, and so without the cost of a throw. The
`#else` branch keeps the program working with an older standard library,
with the chapter's version, which prints the same.

## Exercise 4

`internal.cc`, with `set_abort_on_internal_error(true)` before the
withdrawal:

```{.cpp include="examples/solutions/errors-ex4.cc"}
```

It logged the internal error, as before:

```
ERROR ... [shard 0:main] app - negative balance -50 after withdrawing 150, at: 0x585de8 ...
Aborting on shard 0, in scheduling group main. Backtrace: ...
```

and then aborted, with exit status 134, which is 128 plus `SIGABRT`'s
number, 6. It didn't print `the withdrawal failed, and the program goes
on`, nor anything about the deposit: `on_internal_error()` didn't
throw, it ended the program.

Aborting is what you want where finding the bug matters more than
staying up: in tests, and in debug builds, where an abort stops the
program at the bug, with a core dump of everything as it was, and a
test that passes despite an internal error is a test that lies.
ScyllaDB's test runner, for example, starts its servers with internal
errors set to abort. It's what
you don't want in production, which is why throwing is the default: an
internal error found in one request should fail that request, and be
logged, while the server goes on serving everyone else. In between, a
production deployment might abort on purpose for a while, to collect a
core dump of a bug that can't be reproduced otherwise.

## Exercise 5

`internal.cc`, printing the exception's `what()`:

```{.cpp include="examples/solutions/errors-ex5.cc"}
```

It printed

```
the withdrawal failed: negative balance -50 after withdrawing 150 Backtrace: 0x585f68 0x586826 ...
```

The message that `on_internal_error()` throws ends with the backtrace
of where it was called. Built with `-g`, and its addresses given to
`seastar-addr2line -e errors-ex5`, the backtrace decodes to Seastar's
own functions that make the exception, and then:

```
seastar::on_internal_error(seastar::logger&, std::basic_string_view<char, std::char_traits<char>>) at src/core/on_internal_error.cc:59
withdraw(long&, long) at examples/solutions/errors-ex5.cc:20
 (inlined by) operator() at examples/solutions/errors-ex5.cc:46
...
```

It points to `withdraw()`, at line 20, the call of
`on_internal_error()`, and inlined into it, to line 46 of `main()`'s
lambda, the call `withdraw(balance, 150)`, the caller that forgot to
check. That's the point of putting the backtrace in the exception: a
`catch` far from the bug, which only logs `what()`, still tells where
the bug was found, and how the program got there. The backtrace is
taken when the exception is made, so it shows the stack of the code that
detected the problem, not of the code that caught it.
