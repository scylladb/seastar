# Futures and promises {#sol:futures-and-promises}

## Exercise 1

Here is `states.cc`, with one more line, which asks for `answer_later()`'s
result before awaiting anything:

```{.cpp include="examples/solutions/futures-and-promises-ex1.cc"}
```

It aborts:

```
src/core/future.cc:240: void seastar::internal::future_base::do_wait(): Assertion `thread` failed.
terminate called without an active exception
Aborting on shard 0, in scheduling group main. Backtrace: ...
```

(Even the lines that `describe()` printed before don't appear: they were
still in the buffer of the program's standard output, which an abort
doesn't flush.)

`get()` on a future that isn't available must wait for it, and it can
wait only by stopping the code that called it, in the middle, until the
result arrives. A coroutine can stop in the middle because the compiler
turned it into a state machine, whose state lives in its frame: `co_await`
returns to the reactor, and the reactor resumes the coroutine later, at
the same point. A call of `get()` is an ordinary function call, and to
stop in the middle of one, the whole stack of calls under it, up to the
reactor, would have to be put aside and resumed later. Only a *Seastar
thread*, which has a stack of its own, can do that: there, `get()` switches
from the thread's stack back to the reactor's, and switches back when the
result is ready. That's what `do_wait()` tries, and its assertion checks
that the caller is in a Seastar thread. Here it isn't, so the program
stops, rather than corrupt the stack. [Chapter](#ch:threads) shows Seastar
threads. In a coroutine, `get()` is only for a future that is known to be
available, after checking with `available()`, or after `as_future()`.

## Exercise 2

The loader's `load()` now fails the first time it's called, and the
loader has a choice: keep the failed load, or forget it, with
`load_or_forget()`:

```{.cpp include="examples/solutions/futures-and-promises-ex2.cc"}
```

Asking for the same key three times, one after the other, the loader of
`shared.cc`, `--variant broken`, prints

```
1 failed: the disk is busy
2 failed: the disk is busy
3 failed: the disk is busy
1 loads
```

One load, which failed, and three failed requests. The `shared_future`
that the first request put in the map holds the load's result, which is
the exception, and gives every later request the same exception, for as
long as the key is in the map: the loader caches the failure, just as it
caches a value. That's right for a value, and wrong for a failure, which
is often temporary, as here.

With `--variant fixed`:

```
1 failed: the disk is busy
2 got the value of x
3 got the value of x
2 loads
```

`load_or_forget()` awaits the load with `coroutine::as_future()`, which
gives it the future, available or failed, without throwing, and when it
failed, erases the key from the map, before passing the failure on with
`co_await std::move(f)`. The first request still gets the failure; the
second finds no key, loads again, and succeeds, and the third gets the
cached value.

There is a subtlety. `load_or_forget()` erases the key, and `get()` puts
it in the map, with `emplace()`, after `load_or_forget()` returns its
future. Here, `load()` always sleeps before it can fail, so by the time
it fails, `emplace()` has run, and the erase finds the entry. A `load()`
that could fail at once, before its first `co_await`, would make
`load_or_forget()` erase the key before it was put in the map, and the
failure would then stay there after all. A careful loader would erase
only an entry that holds this very load, or put the entry in the map
before starting the load.

## Exercise 3

An `event` holds a `shared_promise<>`, whose futures `wait()` gives out;
`signal()` fulfills it, and replaces it with a new one at the same time:

```{.cpp include="examples/solutions/futures-and-promises-ex3.cc"}
```

It prints

```
first signal
waiter 1 woke up
waiter 2 woke up
second signal
waiter 3 woke up
```

Waiters 1 and 2 got futures of the first promise, and woke on the first
signal; waiter 3 started after it, got a future of the second, and woke
on the second.

The hint points to the one step that matters: `std::exchange()` puts a
new, unfulfilled promise in `_p`, and returns the old one, which
`set_value()` then fulfills. Without the replacement, the first signal
would fulfill `_p` itself, and every later `wait()` would get a future of
a promise that was already fulfilled, which is available at once: waiter
3 wouldn't wait at all, and a second `set_value()` on the same promise
would be an error. With it, each promise is fulfilled once, and each
waiter waits for the first signal after it called `wait()`. (Seastar's
`condition_variable`, in [chapter](#ch:coordination), does this job, and
more.)

## Exercise 4

With 5 milliseconds instead of 50:

```{.cpp include="examples/solutions/futures-and-promises-ex4.cc"}
```

it prints only

```
dropped the future
done
```

and exits with status 0, without the warning about an ignored exception.
`fail_later()` sleeps 10 milliseconds, and the program ends after 5: when
the main coroutine finishes, `app_template::run()` stops the reactor, and
the sleeping coroutine is never resumed. It doesn't print its line, and
doesn't throw, so there's no exception to report. It's simply abandoned,
in the middle, with its frame never destroyed.

So a program that leaves operations running in the background when it
exits loses them silently: no error, and no warning. Whatever they were
doing, writing a file, replying to a client, is just not done. A server
must therefore wait for its background work before it exits, which takes
a way to know what's running, and to wait for it:
[chapter](#ch:coordination)'s gates, which [chapter](#ch:clean-shutdown)
uses to stop Shoal cleanly. (And the frame that was never destroyed is a
memory leak, which a `debug` build's LeakSanitizer reports at exit, as
[chapter](#ch:memory-leaks) shows with a program much like this one.)

## Exercise 5

At `-O0`, `perf report` of `cost.cc` shows dozens of small functions,
each taking 1.5 to 3% of the time: the constructor of the awaiter that
`co_await` creates, `future::~future()`, `future_state_base::available()`,
`any::failed()` and `any::has_result()`, `uninitialized_wrapper`'s
constructor and its `uninitialized_get()`, `detach_promise()`, and more.
At `-O2`, none of them appears. The profile is then made of a few larger
functions: the loop that makes the calls, the functions it calls,
`reactor::add_task()`, and the allocator's `malloc` and `free_sized`.

The small functions went into their callers: the optimizer inlined them.
A future's machinery is built from many tiny functions, each of which
checks a flag or moves a value, and is defined in a header, so that the
compiler can see its code wherever it's used. At `-O0`, each is a real
call, with its own stack frame, arguments and return, which costs more
than the work it does; at `-O2`, the compiler copies its few instructions
into the caller, and then simplifies them together with the code around
them, often to nothing: a check whose answer the compiler can see is
removed. That's why, with GCC, `make_ready_future()` costs 42 nanoseconds at
`-O0`, and 2 at `-O2`, as the chapter's table of optimization levels
shows. The
functions that remain at `-O2` are the ones that can't be inlined: those
in the compiled Seastar library, such as `add_task()`, and the
allocator. (On the laptop, `perf report` showed a separate profile for
each kind of core; the one with most of the samples is the one that ran
the program.)
