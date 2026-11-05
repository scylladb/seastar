# Coordination {#sol:coordination}

## Exercise 1

The solution is a patch to Shoal version 6,
`examples/solutions/shoal-brpop.patch`. Its tests came first, and pass
against Valkey:

```{.python include="examples/solutions/shoal-brpop.patch" file="test_solutions.py"}
```

They answer the exercise's question. Clients blocked on the same key are
served in the order in which they blocked, whichever command each one
used, and each takes the element at its own end of the list: when a
client blocked in `BRPOP` and then one blocked in `BLPOP` wait for a key,
and another client pushes `a`, `b` and `c`, the first gets `c`, the last
element, the second gets `a`, the first, and `b` stays in the list. With
the `BLPOP` client blocked first, they get the same elements: the order
decides who is served first, and the command decides from which end.

So a waiting client must remember which end it pops from, and
`blocked_pop` gets one more member:

```{.cpp include="examples/solutions/shoal-brpop.patch" file="commands.hh" snippet="blocked-pop"}
```

`serve_blocked()`, which gives the elements of a push to the waiting
clients, takes each element from the end that its waiter asked for:

```{.cpp include="examples/solutions/shoal-brpop.patch" file="commands.cc" snippet="serve-blocked"}
```

And `BLPOP` becomes `bpop()`, with a parameter that says which end to
pop from, both when the list has an element at once, and in the
`blocked_pop` it waits with; `BLPOP` and `BRPOP` call it:

```{.cpp include="examples/solutions/shoal-brpop.patch" file="commands.cc" snippet="blpop"}
```

with one more line in the table of commands:

```cpp
    {"brpop", {.arity = -3, .run_long = brpop}},
```

Everything else, the timeout, the client hanging up, and the lines of
waiting clients for each key, is shared. With these, all 71 tests pass,
against Valkey and against the patched Shoal.

## Exercise 2

The patch `examples/solutions/shoal-all-args-wait.patch` changes only
`admit()`, so that every admission waits for its units:

```{.cpp include="examples/solutions/shoal-all-args-wait.patch" file="resp.cc" snippet="admit"}
```

This client sends three requests at once, each an `RPUSH` of two
elements of 40 MB, on connections of its own:

```{.python include="examples/solutions/coordination-ex2.py"}
```

With `--max-request-memory 104857600`, 100 MB, none of the three got a
reply in 10 seconds; each client's send timed out, because Shoal had
stopped reading. Shoal version 6 replied to all three at once, with
`:2`, `:4` and `:6`, the list's length after each push.

The three requests deadlocked. Each request's first element waited for
40 MB of units; two of them got theirs, and hold 80 MB of the 100 MB,
and the third waits for its first 40 MB. Then each of the first two
reached its second element, and waited for 40 MB more, while holding
its first 40 MB. Only 20 MB are left, and every request waits for 40 MB:
the third for units that the first two hold, and each of the first two
for units that the other one holds, and the third would. Nobody can go
on, so nobody will give back what it holds. Each request alone needs
only 80 MB, less than the limit, and two requests alone would have
deadlocked too.

That's the classic condition for a deadlock: waiting for a resource
while holding another of the same kind, which others wait for. Version
6 breaks it with the rule that the exercise asked to change: only a
request's *first* admission waits, when the request holds nothing
yet. Later arguments take their units without waiting, even beyond the
limit, so a request that holds units never waits for more, and always
finishes, and gives them back. The limit is then sometimes exceeded,
here by up to 60 MB, which is the price of never deadlocking; a request
could also announce all its arguments' sizes first and wait once for
the total, but RESP doesn't announce them.

## Exercise 3

`withdraw.cc`, with `withdraw_safely()` holding a `std::mutex`:

```{.cpp include="examples/solutions/coordination-ex3.cc"}
```

It printed the first line, `withdraw: 2 of 2 withdrawals done, balance
-60`, and then hung, its thread asleep. `kill`, which sends SIGTERM,
didn't end it; only `kill -KILL` did. (With the output
redirected to a file, even the first line was lost, still in
`std::println()`'s buffer when the program was killed; `stdbuf -oL`
shows it.)

The first call of `withdraw_safely()` locks the mutex, and then, at its
`co_await`, returns to the reactor, still holding it: the lock is held
by the shard's thread, not by the coroutine, and the thread goes on to
run other work. The other work is the second call, which, on the same
thread, tries to lock the same mutex. A `std::mutex` doesn't know about
coroutines: it blocks the thread until the lock is free, and the thread
that would free it is the one blocked. (Locking a `std::mutex` that the
same thread already holds is, by the standard, undefined behavior; in
practice, the thread waits for itself forever.) The first coroutine
would unlock it when it resumes, but resuming it is the reactor's job,
on this very thread.

SIGTERM doesn't help, for the same reason. Seastar's signal handler
doesn't stop the program by itself: it only notes that the signal
arrived, and the reactor, the next time it polls, runs the function that
the application registered for it, which for `app_template` starts a
clean stop. The reactor never polls again, so nothing runs that
function. That's why a Seastar program must never block its thread:
everything on the shard, even the response to signals, waits for it.
A semaphore of one unit, or Seastar's `shared_mutex`, does the job,
because waiting for it is a future, which gives the thread back to the
reactor.

## Exercise 4

`gate.cc`, with the gate destroyed, without `close()`, while a save
holds it:

```{.cpp include="examples/solutions/coordination-ex4.cc"}
```

Both builds stopped at once, at the end of the block that destroys the
gate, with the same message, and exit status 134:

```
.../seastar/core/gate.hh:105: virtual seastar::gate::~gate(): Assertion
`!_count && "gate destroyed with outstanding requests"' failed.
Aborting on shard 0, in scheduling group main. Backtrace: ...
```

(The line `destroying the gate, without close()` didn't appear: it was
still in the buffer of the standard output, which an abort doesn't
flush.) The assertion is in the gate's destructor, which Seastar checks
in every build, not only in `debug` mode.

Letting the save go on would be worse. The save's `gate::holder` points
to the gate, and when the save ends, 10 milliseconds later, the holder's
destructor tells the gate that one holder fewer is left, by changing the
gate's count, in memory that is no longer the gate's. In a `release`
build, that's a write to freed memory, which corrupts whatever object
has since been put there, and the program fails later, somewhere else,
or not at all, for a while. A `debug` build would catch the write,
with AddressSanitizer, but only when the save ends, long after the
mistake, in code that did nothing wrong. The assertion stops the program
at the mistake itself, the destruction of a gate whose work isn't done,
with a backtrace that points to it: the cheapest moment to find a
lifetime bug.

## Exercise 5

This client subscribes three clients that never read, and then
publishes 1,000 messages of 100 KB, waiting at most 15 seconds for each
reply:

```{.python include="examples/solutions/coordination-ex5.py"}
```

Against Shoal version 6, with `--pubsub-max-total-pending 10000000`,
51 `PUBLISH`es were done, and then the publisher waited, until the client
gave up after 15 seconds. No subscriber was disconnected.

The publisher waits for units of the semaphore that limits the memory of
all the waiting messages, 10 MB, and the units come back only when
subscribers read their messages, which these never do. The
per-subscriber limit, which disconnects a subscriber that is too far
behind, is 32 MB by default, and none of the subscribers ever got near
it: the total limit, divided among three subscribers, stopped the
publisher first, at a few MB each. The first 51 messages got through
because Shoal's units are given back when a message is written to the
socket, and the kernel's socket buffers took some of them. So two
limits that each make sense block each other: the total limit stops the
publisher before the per-subscriber limit can find a culprit, and the
publisher waits forever.

The patch `examples/solutions/shoal-publish-timeout.patch` gives the wait a
timeout of a second, after which it disconnects the subscriber that is
furthest behind, and waits again:

```cpp
    // Don't wait forever: when the wait takes longer than a second,
    // disconnect the subscriber that is furthest behind, whose messages
    // hold the units, and wait again.
    semaphore_units<> units;
    while (true) {
        try {
            units = co_await get_units(
                _memory, std::min(cost * subscribers, _memory_limit),
                std::chrono::seconds(1));
            break;
        } catch (semaphore_timed_out&) {
            disconnect_slowest();
        }
    }
```

```{.cpp include="examples/solutions/shoal-publish-timeout.patch" file="pubsub.cc" snippet="disconnect-slowest"}
```

Now all 1,000 `PUBLISH`es were done, in 3.1 seconds, and Shoal logged
three disconnections, of subscribers with 3.3, 4.9 and 9.9 MB of unsent
messages. Each took one second of waiting: the publisher waited a
second, disconnected the subscriber furthest behind, whose messages then
gave their units back, and went on until the total limit stopped it
again, this time with two subscribers, and then with one. After the
third, nobody was subscribed, and the remaining `PUBLISH`es replied `0`
at once. The patched Shoal passes all of version 6's 67 tests.

A timeout is a blunt cure: it disconnects a subscriber after a second,
whether it's stuck or only slow, and makes every publisher wait that
second first. But it turns a wait that could last forever into one that
can't, which is what matters here. A finer design would notice the
problem without waiting, for example by disconnecting the subscriber
furthest behind as soon as the total limit is reached. Redis limits each
subscriber's unsent messages, with `client-output-buffer-limit`, but not
their total, and so doesn't have this problem, at the price of letting
the total grow with the number of subscribers.
