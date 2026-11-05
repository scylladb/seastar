# Clean shutdown {#sol:clean-shutdown}

## Exercise 1

```{.bash include="examples/solutions/clean-shutdown-ex1.sh"}
```

With `SHOAL6` and `SHOAL7` naming the two versions' programs, it
printed:

```
SigIgn:	0000000000000006
.../shoal/v6/shoal: still running after SIGINT
SigIgn:	0000000000000004
.../shoal/v7/shoal: stopped, with exit status 0
```

`SigIgn` is a mask of the signals that the process ignores, with bit
*n* − 1 for signal *n*. Version 6's 6 is bits 1 and 2, signals 2 and 3:
`SIGINT` and `SIGQUIT`. A shell that runs a script, without job control,
starts each background job with those two signals ignored, so that a
Ctrl-C meant for the script doesn't stop its background jobs too, and a
program inherits that setting. Version 6 sets
`auto_handle_sigint_sigterm` to `false`, so neither Seastar nor Shoal
installs a handler for `SIGINT`, the inherited "ignore" stays, and `kill
-INT` does nothing. Version 7 installs its own handler, with
`handle_signal()`, and installing a handler replaces the "ignore": its
mask is 4, only `SIGQUIT`, and `kill -INT` stops it cleanly. (Version 6
would have stopped on `kill`, which sends `SIGTERM`, a signal that the
shell doesn't ignore.)

## Exercise 2

`periodic.cc`, without the `try` and `catch`:

```{.cpp include="examples/solutions/clean-shutdown-ex2.cc"}
```

It printed

```
sleep(): stopping took 0.9 seconds
WARN  ... seastar - Exceptional future ignored: seastar::sleep_aborted (Sleep is aborted), backtrace: ...
sleep_abortable(): stopping took 0.0 seconds
condition_variable: stopping took 0.0 seconds
```

and exited with status 0, in both a `release` and a `debug` build. The
stop is as quick as it was: the gate still closes at once. Now
`sleep_aborted` ends `save_periodically()`, and a coroutine that ends
with an exception ends all the same: its frame is destroyed, with its
parameters, so the `gate::holder` is destroyed, and the gate counts one
holder fewer. That's why the chapter made the holder a parameter: it's
released however the coroutine ends. What's new is the warning. The
exception went into the coroutine's future, and `stop_after_a_while()`
started the work with `(void)`, so nobody looked at that future: when
it's destroyed, still holding an exception, Seastar logs it as ignored,
as [chapter](#ch:futures-and-promises) showed. The `catch` was the
coroutine's way of saying "an abort is how I'm meant to end", which
keeps the expected ending out of the log of unexpected ones.

## Exercise 3

```{.cpp include="examples/solutions/clean-shutdown-ex3.cc"}
```

It printed `subscription engaged: false, function called: false`. The
function isn't called, and `subscribe()` returns an empty subscription,
which converts to `false`: an abort source calls its subscribers only
when the abort is requested, and a function that subscribes later has
missed that moment, which won't come again. A caller that may subscribe
late must check `abort_requested()` itself, or test the subscription it
got.

In Shoal's `handle_connection()`, the subscription `stop_reading` shuts
down the connection's input when Shoal starts to stop, which ends the
loop that reads the client's requests. It comes before the first
`co_await`, and the accept loop checks `stopping` after each `accept()`,
so no stop can slip in between. If it came after a `co_await`, a stop
requested during that wait would find the connection not yet subscribed,
and its later `subscribe()` would get an empty subscription, without the
call. The connection would go on reading: for a client that sends
nothing, forever. Shoal's `connections.close()` would wait for it, and
the grace period wouldn't help, since giving up only stops writing, and
this connection isn't writing anything. Shoal would never stop.

## Exercise 4

The patch to Shoal version 7, `examples/solutions/shoal-second-signal.patch`,
changes only the signal handlers: the first signal requests `stopping`,
as before, and a second one requests `giving_up`, which the grace
period's timer would have requested later:

```{.cpp include="examples/solutions/shoal-second-signal.patch" file="main.cc" snippet="signals"}
```

A second request of the same abort source would do no harm, since
`request_abort()` does nothing the second time; the checks only keep the
log message for the case that needs it. Requesting `giving_up` early is
enough, because everything else is already there: each connection's
`stop_writing` subscription shuts down the output, the write that waits
for a client that doesn't read fails, the connection ends, and the
gate closes.

The patch's test, which passes against Valkey and against the patched Shoal,
sends two signals a tenth of a second apart, with a client that doesn't
read its replies, and expects the server to exit well within `./run`'s
grace period of a second:

```{.python include="examples/solutions/shoal-second-signal.patch" file="test_solutions.py"}
```

Against version 7 itself, it fails, as it should: version 7 exits only
when the grace period ends.

This script measures the exercise's case, with a grace period of 100
seconds:

```{.python include="examples/solutions/clean-shutdown-ex4.py"}
```

| | After the first `SIGINT` | After the second |
|---|---|---|
| Shoal version 7 | still running after 5 s | still running after 10 s |
| The patched Shoal | still running after 5 s | exited with status 0 after 0.03 s |

After one signal, both wait, as they should: the client has requests
whose replies Shoal can't write, and Shoal waits up to its grace period
for them. After the second, version 7 still waits out the 100 seconds,
while the patched Shoal gives up at once, and exits cleanly, in 30
milliseconds: giving up is the same clean path as at the end of the
grace period, only earlier.

## Exercise 5

`periodic.cc`, without the subscription in `save_changes()`:

```{.cpp include="examples/solutions/clean-shutdown-ex5.cc"}
```

It printed the first two lines, for `sleep()` and `sleep_abortable()`,
and then hung, at the condition variable's case. `kill -INT`, the
signal that Ctrl-C sends, didn't stop it; only `kill -KILL` did.

`stop_after_a_while()` requested the abort, and then waited for the gate
to close, which waits for `save_changes()`. But `save_changes()` is
inside `changed.wait()`, and nothing will signal `changed`: nothing
changes, and the subscription that turned the abort into a
`broadcast()` is gone. The coroutine checks `stopping` only between
waits, so it never sees the request. An abort source isn't a signal that
reaches every waiting coroutine: it reaches only the operations that
were given it, such as `sleep_abortable()`, and the functions subscribed
to it. Anything else that waits, a condition variable, a semaphore
without an abort source, a read, must be connected to it by hand, or it
waits on.

Ctrl-C can't help either, for the reason the chapter gave: the program
uses `app_template`'s default handlers, which start Seastar's own
shutdown, and that shutdown first waits for the function given to
`run()`, which waits for the gate, which waits for `save_changes()`. The
program's own abort source has nothing to do with the signals: nothing
connects `SIGINT` to it, and if something did, it would only request an
abort that `save_changes()` doesn't hear.
