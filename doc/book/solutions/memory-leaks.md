# Finding memory leaks {#sol:memory-leaks}

## Exercise 1

Built against a `debug` Seastar, `abandon.cc` reported what the chapter
showed: four indirect leaks, 4,392 bytes in all, the vector's 4,000
bytes, the coroutine's frame, and the sleep's timer, promise and
continuation, and an exit status of 1.

Here it is, stopping cleanly:

```{.cpp include="examples/solutions/memory-leaks-ex1.cc"}
```

It printed `main is done`, LeakSanitizer reported nothing, and the exit
status was 0, in a `debug` build and in a `release` one.

The leak was a coroutine that was still waiting when the program ended.
Now nothing is left waiting. The main function requests the abort,
which ends `sleep_abortable()` at once, with `sleep_aborted`; the
coroutine catches it and returns, so its frame is destroyed, and with
it the vector, while the sleep's timer, promise and continuation are
freed when the sleep ends. The coroutine's `gate::holder` is a parameter,
destroyed with the frame, and `close()` waits for it: the main function
returns only after the coroutine has ended, so the program exits with
everything freed. Either half alone wouldn't do. Without the abort
source, `close()` would wait for the hour-long sleep; without the gate,
the main function might return before the coroutine had run far enough
to end, and nothing would wait for it.

So a leak report at exit, from a Seastar program, is often not about
memory at all: it's the sign of background work that the program didn't
stop, the same omission that [chapter](#ch:clean-shutdown) cures for
Shoal. Making a program stop cleanly makes its leak reports mean
something: what LeakSanitizer reports then is a real leak.

## Exercise 2

The program, `examples/solutions/memory-leaks-ex2.cc`, makes a session
of each kind, and drops both. The first fix gives the callback a plain
pointer:

```{.cpp include="examples/solutions/memory-leaks-ex2.cc" snippet="plain"}
```

The second gives it a `weak_ptr`, which the session hands out because it
derives from `weakly_referencable`; the `weak_ptr` doesn't keep the
session alive, and becomes null when the session is destroyed, which the
callback checks:

```{.cpp include="examples/solutions/memory-leaks-ex2.cc" snippet="weak"}
```

Built against a `debug` Seastar, it printed `main is done`,
LeakSanitizer reported nothing, and the exit status was 0. Of the two,
the plain pointer is right here, since the timer can't outlive the
session; a `weak_ptr` costs a little more, and is for a callback kept
somewhere else, such as a timer of the whole server, which might run
after the session is gone.

## Exercise 3

The program, `examples/solutions/memory-leaks-ex3.cc`, is `stuck.cc` with
a timeout on the wait: `condition_variable::wait()` with a duration and
a predicate fails with `condition_variable_timed_out` when the duration
passes first, which the request catches, and ends:

```cpp
try {
    co_await replies.wait(std::chrono::seconds(1),
                          [this, id] { return answered.contains(id); });
    answered.erase(id);
} catch (condition_variable_timed_out&) {
    // The reply was lost: give up on it.
}
```

Since a lost reply now costs a second, the program runs 40 rounds, a
tenth of a second apart, and prints every fifth:

```
after round 5: 26068 KB allocated, 3129 live objects
after round 10: 28168 KB allocated, 5130 live objects
after round 15: 28156 KB allocated, 5130 live objects
after round 20: 28244 KB allocated, 5130 live objects
after round 25: 28256 KB allocated, 5130 live objects
after round 30: 28268 KB allocated, 5130 live objects
after round 35: 28184 KB allocated, 5130 live objects
after round 40: 28220 KB allocated, 5130 live objects
```

The live objects grow for the first ten rounds, about a second, while
nothing has timed out yet, and then stay the same: from then on, every
round's lost requests replace the ones that time out, and about 800
requests, eight rounds' worth, are always waiting. So a repeat test of a
program with timeouts must run longer than its longest timeout, or it
mistakes waiting work for a leak.

## Exercise 4

The patch, `examples/solutions/shoal-repeat-more.patch`, changes no
code. It adds the chapter's test, with two more kinds of clients in the
workload:

```{.python include="examples/solutions/shoal-repeat-more.patch" file="test_solutions.py" snippet="more"}
```

The pipelined requests read a list of 1,000 elements, which the test
makes first, so that each reply is long, and many are still waiting to
be written when the client hangs up. Shoal 16 doesn't leak with them:

```
objects left after 10 more rounds, by shard: {0: 1, 1: 0}
```

So Shoal frees what these clients leave behind: a request it was
reading, and replies it was still writing. Had it kept them, each round
would have left at least a hundred more objects, which the test, as the
chapter's planted leak showed, would have reported.
