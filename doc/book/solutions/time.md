# Time {#sol:time}

## Exercise 1

The solution is a patch to Shoal version 5,
`examples/solutions/shoal-pexpire.patch`. Its tests came first, and pass
against Valkey:

```{.python include="examples/solutions/shoal-pexpire.patch" file="test_solutions.py"}
```

`PEXPIRE` differs from `EXPIRE` only in the unit of its time, and `PTTL`
from `TTL` only in the unit of its answer, so the patch turns the old
functions into two that take the difference as a parameter:

```{.cpp include="examples/solutions/shoal-pexpire.patch" file="commands.cc" snippet="expire"}
```

with two more lines in the table of commands:

```cpp
    {"pexpire", {.arity = 3, .run = pexpire}},
    {"pttl", {.arity = 2, .run = pttl}},
```

The limit on the time is the same, 100 years, in either unit, and so is
the error message, with the command's name. With these, all 58 tests
pass, against Valkey and against the patched Shoal.

To see what `PTTL` returns right after `PEXPIRE key 1000`, this script
asks 1,000 times, sending the two commands one after the other, and
then both in one pipeline:

```{.python include="examples/solutions/time-ex1.py"}
```

| `PTTL` right after `PEXPIRE key 1000` | Valkey 9.0.6 | Shoal |
|---|---|---|
| one after the other | 1000 (986 times), 999 (14 times) | 999 (1,000 times) |
| in one pipeline | 1000 (1,000 times) | 1000 (999 times), 999 (once) |

Valkey usually says 1000, and Shoal, unless the two commands come in one
pipeline, always says 999. The difference is in the clocks. Valkey
keeps expiration times in whole milliseconds: `PEXPIRE` stores the
current millisecond plus 1,000, and `PTTL` subtracts the current
millisecond. Two commands a round trip apart, a few tens of
microseconds, usually fall in the same millisecond, and the answer is
1000; it's 999 only when the clock's millisecond changed between them,
which happened 14 times in 1,000.

Shoal's `lowres_clock` counts in nanoseconds, but its value changes only
when the reactor updates it, once each time it polls, before it runs the
tasks that the poll found. A command that comes in its own round trip
is executed after a later poll than the `PEXPIRE` was, so the clock has
moved on by a few microseconds: 999.98 milliseconds are left, and
`duration_cast<milliseconds>` truncates that to 999. The two commands of
a pipeline arrive together, and are executed between the same two polls,
so both see the same time, and exactly 1,000 milliseconds are left.

Neither is wrong: a key's remaining time is never exact, and changes
while the reply is on its way. But a client that compares the two
servers sees a difference, and a test that expects `PTTL` to return
exactly 1000 would pass against Valkey and fail against Shoal. (That's
why the patch's test accepts anything a little below the expiration
time.) Shoal could round to the nearest millisecond, rather than down,
to answer 1000 too, which is what Valkey's clock effectively does.

## Exercise 2

The solution is a patch to Shoal version 5,
`examples/solutions/shoal-idle-timeout.patch`. `serve()`, which reads a
client's requests, gets the socket and the timeout, and arms a timer
before each read:

```{.cpp include="examples/solutions/shoal-idle-timeout.patch" file="main.cc" snippet="serve"}
```

`handle_connection()` passes them on, and `main()` adds the option:

```cpp
        ("timeout", bpo::value<unsigned>()->default_value(0),
         "close a client's connection after it has sent nothing for this "
         "many seconds (0: never)");
```

The timer is rearmed before each read, so it measures the time since the
client's last request, and it's canceled as soon as a request arrives,
so that a request that takes long to execute, or to reply to, isn't
counted as idle time. When the timer expires, its function calls
`shutdown_input()`: the read that waits for the next request then ends
as if the client had closed the connection, the loop ends, and
`handle_connection()` closes the connection as it always does, after
waiting for any writes in progress. The timer lives in `serve()`'s
frame, and its function uses the socket, which lives in
`handle_connection()`'s; both outlive every read, and the timer is
destroyed, which cancels it, when `serve()` returns.

One more case came from checking the tests against Valkey: Redis doesn't
apply the timeout to clients subscribed to Pub/Sub channels, which may
wait for messages as long as they like. So the timer isn't armed while
the client is a subscriber.

Testing it takes a server started with a timeout, which Shoal's usual
tests don't want, so the patch's header asks for one, in a line
`# server options: --timeout 2`, and the tests start both servers,
Valkey and Shoal, with a timeout of 2 seconds. The tests use
plain sockets, to control exactly when anything is sent, and check three
things: that an idle connection is closed, after about the timeout; that
a connection that sends a `PING` every half second stays open, although
it lives four seconds; and that a subscriber stays open:

```{.python include="examples/solutions/shoal-idle-timeout.patch" file="test_solutions.py"}
```

They pass against both servers, and so do all the other tests, 54 in
all. The time to wait for the close needs a generous margin: Valkey
closed an idle connection after 3.0 seconds, in three tries, and Shoal
after 2.0. Valkey counts a client's idle time in whole seconds, and
closes the connection only when it has been idle longer than the
timeout, which, checked periodically, comes to a second more; Shoal's
timer fires when the timeout has passed. The test accepts anything from
half a second before the timeout to 2 seconds after it, which catches a
server that closes too early, or never.

## Exercise 3

`slow_operation()` takes a `std::string&`, and `try_with_timeout()`
passes it a local variable:

```{.cpp include="examples/solutions/time-ex3.cc"}
```

Built normally, it printed

```
timed out
slow_operation(<8 bytes of garbage>ation that timed out) finished
done
```

and exited with status 0. Built against a `debug`-mode Seastar,
AddressSanitizer stopped it, with a "heap-use-after-free" at line 18,
the `println()` in `slow_operation()`, in memory freed at line 32, the
end of `try_with_timeout()`: a region of 232 bytes, the coroutine frame
of `try_with_timeout()`, which holds `name`.

`with_timeout()` doesn't stop the operation when the time runs out. It
returns a future that fails with `timed_out_error` after 20 milliseconds,
while `slow_operation()` goes on running, to the end of its 100
milliseconds of sleep. `try_with_timeout()` catches the exception, and
returns, and its frame, with the string `name`, is freed. Eighty
milliseconds later, `slow_operation()` wakes up, and reads `name`
through its reference, from freed memory. In the `release` build, the
string object in the freed frame still pointed to the string's
characters, which its destructor had freed, and the first 8 of them had
been overwritten, most likely by the allocator, which keeps its own
records in freed memory; so it printed garbage, and then the rest of the
name.

Without the timeout, the bug can't happen: `try_with_timeout()` would
wait for `slow_operation()`, and `name` would outlive it. That's the
convention that [chapter](#ch:object-lifetime) described: the caller
keeps the arguments alive until the future is available. A timeout
breaks the convention, because the caller stops waiting while the
operation still runs: the future it waited for is no longer the
operation's. So an operation given to `with_timeout()` must own
everything it uses, taking its arguments by value, or by a shared
pointer, or it must be stoppable, with an abort source, as the chapter's
`sleep_abortable()` is, so that the caller can stop it, and wait for it
to stop, before the arguments are destroyed.

## Exercise 4

Two copies of Shoal version 5, each with an option `--round-expiry`,
which rounds every expiration time up to a whole second, so that keys
expire together. Without the option, both pass all of version 5's
tests.

The first, `examples/solutions/shoal-key-timers.patch`, has no tree of
expiration times, and no store-wide timer: each key with an expiration
time owns a `timer<lowres_clock>`, whose function deletes it.

```{.cpp include="examples/solutions/shoal-key-timers.patch" file="commands.hh" snippet="store"}
```

```{.cpp include="examples/solutions/shoal-key-timers.patch" file="commands.cc" snippet="expire-at"}
```

The function checks that the key still has this very timer before it
deletes it: `FLUSHALL` moves the old data out and destroys it gradually,
with its timers still armed, and a key set after the `FLUSHALL` may have
the same name as one of them. Deleting the key destroys the timer, and
the function itself, while it runs. That's allowed, as `delete this` is,
as long as the function uses nothing of its own afterward: the reactor
doesn't touch a timer after calling its function.

Every timer function notes the time, and a run of functions with no gap
of more than 50 microseconds between them is logged when it ends:

```{.cpp include="examples/solutions/shoal-key-timers.patch" file="commands.cc" snippet="run-log"}
```

The second, `examples/solutions/shoal-expiry-rounded.patch`, keeps version
5's tree and its one timer, with the same rounding, and logs, when no
more keys are due, how many calls of the timer's function there were,
how long they took together, and the longest.

This script loads about a million keys with `EX 5`, with
`valkey-benchmark`, into Shoal on one core, and shows the log after
they expire:

```{.bash include="examples/solutions/time-ex4.sh"}
```

Each load took 0.7 to 0.9 seconds, and stored about 995,000 keys. In
four runs of each copy:

| | Expiring all the keys | The longest time without a break |
|---|---|---|
| A timer for each key | 238–245 ms | 238–245 ms |
| Version 5's one timer | 199–211 ms | 0.62–1.05 ms |

With a timer for each key, the keys that expired in the same second
expired in one run of timer functions. In these four runs, that was all
of them: the reactor ran 995,000 functions, a quarter of a second,
without polling, without running a task, without answering a client.
(When the keys were loaded in two parts, 1.2 seconds apart, there were
separate runs, about a second apart, each as long as its second's keys
made it.) With version 5's one timer, the same keys took about a
thousand calls, of at most a millisecond each, and other work could run
between them. The total time is similar; what differs is how it's
divided.

The reason is in how Seastar runs timers. When the reactor polls its
timers, it takes *all* those that are due, and calls their functions one
after the other, in one loop; a timer function can't yield, and the loop
doesn't stop for a task quota. That's fine for timers that are few, or
spread out. But timers that come due together, a million of them,
become one long stall, which no timer function can split, since each one
is short. Version 5's single timer controls its own work instead: it
deletes a batch, and rearms itself to run again after other work, which
splits the expiration into pieces as small as it likes. This is the
general rule for work that can grow with the data: it must be done by
something that can stop in the middle, a coroutine that yields, or a
timer that deletes a batch at a time, not by a crowd of small pieces that
the reactor runs all together. And a timer per key costs memory too: a
`timer<lowres_clock>` is 96 bytes, against 48 for version 5's
`expiration`.

(The stall detector isn't the tool for this, as the exercise warned: by
default it reports only a few stalls a minute, and loading a million
keys stalls too, when the hash table grows. In an earlier trial, the
five stalls it reported were all from loading. Logging from the timer
function itself measures exactly the runs.)
