# Finding memory leaks {#ch:memory-leaks}

A server runs for weeks or months, so memory that it fails to free, a
little on every request, adds up until the server runs out. This chapter
is about finding such leaks. They come in two kinds, which need
different tools. Memory that the program lost every pointer to can be
found when the program exits, by LeakSanitizer, but only in a program
that exits cleanly, as [chapter](#ch:clean-shutdown) made Shoal do.
Memory that the program still points to, but will never use again, can
be found only while the program runs, by watching the memory it uses
grow.

## LeakSanitizer {index="LeakSanitizer"}

A *memory leak* is memory that a program allocated, and never freed,
because it lost every pointer to it. A server that leaks a little memory
on every request runs out of memory after days or weeks, and the leak
is hard to find by reading the code. [*LeakSanitizer*]{index="LeakSanitizer" .main} finds it. It's
part of AddressSanitizer, which [chapter](#ch:object-lifetime) used to
find the use of freed memory, so it's on in every Seastar program built
against a `debug` Seastar. When the program exits, LeakSanitizer
searches the memory the program can still reach (global variables, the
stacks, the registers, and every block reachable from those) for
pointers to each block that was never freed. A block that nothing
points to is a leak, and LeakSanitizer reports it, with the place where
it was allocated, and makes the program exit with a status of 1, so that
a test that leaks fails. It works in `debug` mode because there, as
[chapter](#ch:object-lifetime) explained, Seastar uses the system's
memory allocator, whose allocations the sanitizers track; a `release`
build uses Seastar's own allocator, and has no LeakSanitizer.

A program that exits while work is still running leaks that work's
memory. [Chapter](#ch:clean-shutdown) explained why a server should
instead stop its background work, and wait for it, before it exits; here
is what happens when a program doesn't. A coroutine runs in the
background, and the main function ends without waiting for it:

```{.cpp include="examples/memory-leaks/abandon.cc" snippet="abandon"}
```

Built against a `release` Seastar, it prints "main is done" and exits
with a status of 0. Against a `debug` Seastar, it ends with this report
(shortened here):

```
==292595==ERROR: LeakSanitizer: detected memory leaks

Indirect leak of 4000 byte(s) in 1 object(s) allocated from:
    ...
    #7 in std::vector<int, std::allocator<int> >::vector(unsigned long, ...)
    #8 in background abandon.cc:14
Indirect leak of 152 byte(s) in 1 object(s) allocated from:
    #0 in malloc
    #1 in seastar::internal::coroutine_allocators::operator new(unsigned long)
    #2 in background() abandon.cc:13
Indirect leak of 136 byte(s) in 1 object(s) allocated from:
    ...
    #2 in seastar::future<void> seastar::sleep<...>(...)
    #3 in background abandon.cc:17
Indirect leak of 104 byte(s) in 1 object(s) allocated from:
    ...
    #6 in seastar::future<void> seastar::future<void>::finally<...>(...)
    #7 in seastar::future<void> seastar::sleep<...>(...)
    #8 in background abandon.cc:17

SUMMARY: AddressSanitizer: 4392 byte(s) leaked in 4 allocation(s).
```

and an exit status of 1. The 4,000 bytes are the vector's 1,000 `int`s,
allocated at line 14; the 152 bytes, allocated by the operator that
allocates coroutine frames, where `background()` is called (line 13), are
the coroutine's frame; and the other two blocks were allocated by
`sleep()`, which allocates its timer, together with the promise that the
timer fulfills (136 bytes), and a continuation, created by `finally()`,
that owns them until the sleep ends (104 bytes). The debugging
information that GCC produced attributes this call of `sleep()` to line
17, the coroutine's closing brace, rather than line 15, where the
`co_await` is. Inside a coroutine, the line numbers can be off like
this, so trust the function names on the stack more. The program
exited while the coroutine was
still suspended in its sleep. Nothing destroys a suspended coroutine
from the outside, so its frame, and everything it owns, was never
freed.

All four leaks are *indirect*. LeakSanitizer calls a block a *direct*
leak when no pointer to it remains anywhere, and an *indirect* leak when
pointers to it remain, but only in blocks that are themselves leaked.
Here every block has a pointer to it from another leaked block
([Figure](#fig:leak-cycle)), so the blocks must form a cycle: the sleep's continuation owns the timer and
its promise, and the promise points back to the continuation that waits
for it, which points on to the coroutine. Every block in a cycle has a
pointer to it, so all are indirect. Don't dismiss indirect leaks as less
important: a leaked cycle is reported only this way, with no direct leak
to draw your attention.

![The four blocks that `abandon.cc` leaks. The timer's promise points to
the continuation that waits for it, which owns the timer, and points on
to the coroutine that waits for the sleep, whose frame owns the vector.
Nothing the program can still reach points to any of them, but each has
a pointer to it from another, so all four are indirect
leaks.](figures/leak-cycle.svg){#fig:leak-cycle}

Real leaks can hide among such reports. A program that never stops its
background work is reported to leak every time, and its developers soon
learn to ignore the report, or turn LeakSanitizer off (with the
environment variable `ASAN_OPTIONS=detect_leaks=0`), and then a real
leak goes unnoticed. A program that stops cleanly, and frees everything,
exits silently, and any report is worth reading. Shoal version 7, which
[chapter](#ch:clean-shutdown) made stop cleanly, built against a `debug`
Seastar, passes all its tests, including the tests
that stop it, which check that it exits with a status of 0: had it
leaked anything, LeakSanitizer would have made it 1. Exercise 1 makes
`abandon.cc` stop cleanly.

### A pointer to itself {index="memory leak!cycle of shared pointers"}

A Seastar program also leaks through a pattern that it uses all the
time: a callback that holds a shared pointer to the object that it works
on, so that the object lives as long as the callback might run. When the
callback is stored in the object itself, the object can never be freed:

```{.cpp include="examples/memory-leaks/cycle.cc" snippet="cycle"}
```

Built against a `release` Seastar, it prints "main is done" and exits
with a status of 0. Against a `debug` Seastar (shortened again):

```
==2435023==ERROR: LeakSanitizer: detected memory leaks

Direct leak of 136 byte(s) in 1 object(s) allocated from:
    ...
    #2 in seastar::make_lw_shared<session>() shared_ptr.hh:432
    #3 in start_session() cycle.cc:25
Indirect leak of 1000 byte(s) in 1 object(s) allocated from:
    ...
    #8 in session::session() cycle.cc:19
    ...
    #12 in start_session() cycle.cc:25

SUMMARY: AddressSanitizer: 1136 byte(s) leaked in 2 allocation(s).
```

The session, 136 bytes with its `lw_shared_ptr`'s count, is a *direct*
leak, though a pointer to it remains: that pointer is the copy of `s`
that the timer's callback captured, and `noncopyable_function`, which
[chapter](#ch:time) explained, keeps a small lambda inside itself, so
the pointer is inside the session's own memory. A block that only points
to itself is reachable from nothing, so LeakSanitizer counts it as
direct; the buffer, which only the leaked session points to, is
indirect. Cancelling the timer didn't help: it stops the callback from
running, but the callback, and its pointer, are still there. Here the
callback didn't need a shared pointer at all: the timer is a member of
the session, and destroying the session destroys the timer, and cancels
it, so the callback can never run after the session is gone, and can
capture a plain pointer. Where a callback may outlive the object, it
must hold a pointer that doesn't keep the object alive, or the cycle
must be broken explicitly, when the object is done (exercise 2).

### Objects that weren't closed {index="memory leak!objects not closed"}

[Chapter](#ch:clean-shutdown) listed the objects that must be closed,
or stopped, before they're destroyed: output streams, gates, `sharded<>`
services and Seastar threads. Most of them don't leak when that's
forgotten: they abort, at once, in every build, with an assertion that
names the mistake, which is better than a leak, because a test can't
miss it. What leaks is the work that an unclosed object leaves behind,
as `abandon.cc` did: a fiber that nobody waits for, and that is still
suspended when the program exits.

### Suppressions {index="LeakSanitizer!suppressions"}

Not every report is a leak in your code. A library that the program
uses may allocate memory that it never frees, on purpose, or keep it
where LeakSanitizer can't see the pointers to it. Once that happens,
every run of every test reports it, and, as explained above, a report
that's always there teaches people to ignore reports, or to turn
LeakSanitizer off. A *suppression* tells LeakSanitizer to ignore such a
leak, and only it. ScyllaDB has one: since a Seastar update in 2026
brought in the LTTng-UST library, for Seastar's tracepoints
([chapter](#ch:observability)), every exit of a `debug` ScyllaDB, even
`scylla --version`, reported memory that the dynamic loader allocated
for LTTng-UST, and kept only where LeakSanitizer doesn't look. Redpanda
suppresses several, among them two in Seastar itself: a queue of
Seastar's `alien` interface, which stores its pointers in a form that
LeakSanitizer doesn't recognize as pointers, so that what they point to
seems leaked, and one in Seastar's connection balancer, `conntrack`,
where Seastar's issue <https://github.com/scylladb/seastar/issues/738>,
still open, reports a leak too.

A suppression file has a line for each leak to ignore, `leak:` and a
pattern, which LeakSanitizer looks for in the names of the functions,
files and libraries on the stack where the leaked block was allocated;
lines that start with `#` are comments. The environment variable
`LSAN_OPTIONS` names the file. For `cycle.cc`:

```
$ cat lsan.supp
# The session that start_session() makes keeps itself alive.
leak:start_session
$ LSAN_OPTIONS=suppressions=lsan.supp ./cycle
-----------------------------------------------------
Suppressions used:
  count      bytes template
      2       1136 start_session
-----------------------------------------------------

main is done
```

The exit status is 0, and LeakSanitizer still says what it ignored, and
how much, so that a suppression that starts to hide more than it should
shows. A pattern should be as narrow as it can be: one function or
library, never a whole program, and with a comment that explains why the
leak isn't a leak, or which issue will fix it, as ScyllaDB's one
suppression has, a paragraph of it, and two of Redpanda's four. ScyllaDB's
installation script wraps each of its programs in a script that sets
`LSAN_OPTIONS`, and Redpanda's test runner sets it for its tests.

## Memory that keeps growing {index="memory leak!of reachable memory"}

A silent exit doesn't prove that a program has no leaks. As
explained above, LeakSanitizer reports only memory that nothing points
to anymore. Memory that the program still points to, but will never use
again, isn't reported, and a server can run out of that kind just as
well: a map with an entry for each client, which isn't erased when the
client leaves, grows for as long as the server runs. Seastar adds a
kind of its own: a fiber that
waits forever, on a condition variable or a semaphore that is never
signaled. The waiting fiber's coroutine frame,
and everything the frame owns, is still reachable, from the list of
waiters that the condition variable or semaphore keeps, so it isn't a
leak by LeakSanitizer's definition. One such fiber per request, and
memory grows until the server stops. Then a clean shutdown, like the one in
[chapter](#ch:clean-shutdown), breaks the
condition variable or semaphore, the fiber wakes and ends, and its
memory is freed. So the cleaner the shutdown, the less such leaks show
at exit. Finding them takes a different kind of test. Such a test runs
the same work many times, and checks that the memory in use stops
growing. It runs in a `release` or `dev` build, where Seastar's allocator
counts the memory in use; in a `debug` build, Seastar's statistics about
memory are made up.

Here is such a leak, and such a test, in one program. Requests wait for
their replies on a condition variable, and a bug loses one reply in a
hundred:

```{.cpp include="examples/memory-leaks/stuck.cc" snippet="stuck"}
```

The program runs five rounds of 10,000 requests, and after each, measures
the shard's memory with `memory::stats()`, from
`<seastar/core/memory.hh>`, which [chapter](#ch:memory) explained:
`allocated_memory()`, the bytes in use, and `live_objects()`, the
allocations not yet freed:

```{.cpp include="examples/memory-leaks/stuck.cc" snippet="rounds"}
```

It sleeps a moment before it measures, so that the requests that got
their replies have time to end: our first version measured after a mere
`yield()`, and counted thousands of requests that were still finishing.
Built against a `release` Seastar:

```
after round 1: 22716 KB allocated, 1129 live objects
after round 2: 23820 KB allocated, 1630 live objects
after round 3: 24668 KB allocated, 2130 live objects
after round 4: 25368 KB allocated, 2630 live objects
after round 5: 25936 KB allocated, 3130 live objects
```

Every round leaves 500 more objects: 100 stuck requests, of five
objects each, among them the coroutine's frame and its buffer (four
each, when built by Clang). The
allocated bytes grow too, but less evenly, since the allocator also
keeps memory for its pools; the count of live objects is the more
precise signal. Against a `debug` Seastar, the statistics say
0 every time, and the program exits silently, with a status of 0:
`stop()` breaks the condition variable, the stuck requests wake up with
an exception, and end, and nothing is left for LeakSanitizer to find.

### Where the memory went {index="heap profiling"}

A test that finds memory growing doesn't say what grows. Seastar's heap
profiling, which [chapter](#ch:memory) introduced, does: when Seastar is
built with `configure.py --heap-profiling`, and the program is run with
`--heapprof`, the allocator records the backtrace of a sample of the
allocations, about one in every so many bytes that `--heapprof` sets,
and
[`memory::sampled_memory_profile()`]{index="`seastar::memory::sampled_memory_profile()`"}
returns, for each backtrace whose sampled allocations aren't all freed
yet, how many are still alive, and about how many bytes they stand for.
The program prints the three backtraces that hold the most:

```{.cpp include="examples/memory-leaks/stuck.cc" snippet="profile"}
```

Each sampled allocation stands for at least as many bytes as the
sampling interval, so the bytes are an estimate, and a backtrace is a
list of addresses, which Seastar's script `scripts/seastar-addr2line`,
in Seastar's source, turns into functions and lines, given the
program's executable:

```
$ ./stuck --heapprof 4096 > profile.txt
$ grep "sampled objects" profile.txt
94 sampled objects, 376 KB, allocated at: 0x589aa8 0x45b7b9 0x460e0c ...
1 sampled objects, 128 KB, allocated at: 0x589aa8 0x45b7b9 0x45bbc4 ...
1 sampled objects, 128 KB, allocated at: 0x589aa8 0x45b7b9 0x45bbc4 ...
$ grep "sampled objects" profile.txt | head -1 | seastar-addr2line -e stuck
[Backtrace #0]
...
 (inlined by) std::vector<char, std::allocator<char>>::vector(unsigned long, ...)
 (inlined by) server::request(...) at stuck.cc:30
server::request(int) at stuck.cc:28
 (inlined by) main::'lambda'()::operator()(...) at stuck.cc:55
...
```

The first frames are the allocator's own; the first of the program's is
the request's buffer, at line 30, in `request()`, called at line 55:
the memory that stays allocated is the stuck requests'. The sampling
costs something, and finds only what's sampled, so it's for looking for
a leak that a test found, not for every run. ScyllaDB's build doesn't
enable it, but its gdb script ([chapter](#ch:debugging-and-profiling))
has had a command since 2016, `scylla heapprof`, which reads the samples
from a process, or from a core dump, and can draw them as a flame
graph.

## A repeat test of a server {index="memory leak!repeat test"}

A server's memory can be watched from the outside, through the metrics
that Seastar keeps of each shard's memory, which
[chapter](#ch:observability) served to Prometheus: Shoal, since version
15, serves them on its admin API's `/metrics`, among them
`shoal_memory_malloc_live_objects`, the objects allocated on each shard
and not yet freed. So a repeat test of Shoal can be one more Python
test, which starts a Shoal of its own, with an admin port, runs the same
work again and again, and reads the count before and after:

```{.python include="examples/memory-leaks/test_memory.py" snippet="allocated"}
```

The work should be as varied as the server's, and do what clients do
that a server might forget to clean up after: connect and disconnect,
time out, and hang up in the middle of a wait:

```{.python include="examples/memory-leaks/test_memory.py" snippet="workload"}
```

```{.python include="examples/memory-leaks/test_memory.py" snippet="test"}
```

A few rounds of warm-up come first, since a server's first requests
allocate memory that it keeps, such as its pools, and its tables, which
grow to fit the keys, and don't shrink. After them, Shoal 16 leaves
almost nothing:

```
$ ./run 16 --test ../../examples/memory-leaks/test_memory.py \
      -k test_memory_stops_growing -s
objects left after 10 more rounds, by shard: {0: 1, 1: 0}
```

To see what the test finds, we planted a leak in a copy of Shoal 16:
when the last subscriber of a channel leaves, the channel's entry in the
map of channels, now with an empty set of subscribers, stays:

```diff
@@ -180,9 +180,6 @@
     auto it = _channels.find(channel);
     if (it != _channels.end()) {
         it->second.erase(&c);
-        if (it->second.empty()) {
-            _channels.erase(it);
-        }
     }
 }
```

The leak is harmless otherwise: `PUBLISH` to a channel without
subscribers sends nothing either way. So the copy passes all 122 of
Shoal's tests, in a `release` build and in a `debug` one, and the
`debug` one, after the workload above leaked hundreds of channels, still
exits silently when it's stopped: the map is reachable until then, and
destroyed with the rest. Only the repeat test notices:

```
objects left after 10 more rounds, by shard: {0: 751, 1: 750}
```

Each round's 50 subscribers, of channels of their own, leave three
objects each, spread over the two shards that they connected to. A
repeat test is slower than a test of a feature, and its threshold must
be tuned to what the server keeps for itself, so it's worth running
regularly, not with every change; and the more kinds of work it does,
the more kinds of leaks it can find.

## In production {index="memory leak!in production"}

Some leaks show only in production, with work that no test did. The
same metrics show them there: a shard whose memory in use, or whose live
objects, grows for days, without growing traffic or data to explain it,
leaks. As [chapter](#ch:memory) explained, a Seastar program runs out of
memory when one shard does, so the metrics to watch are each shard's,
not their total, and a leak that's tied to one shard's work, such as a
connection that always lands on it, shows only there. Then a binary
built with heap profiling, run with `--heapprof` for a while, says
where the memory goes, if something reads its samples: the program
itself, which can serve `memory::sampled_memory_profile()` through its
admin API, or gdb, from the running process or its core dump.

## Exercises

1. Build `abandon.cc` against a `debug` Seastar, and run it. Then make
   it stop cleanly: give `background()` a gate holder and an abort
   source, end its sleep with the abort source, and close the gate
   before the main function returns. Does LeakSanitizer still report
   anything? What is the exit status?

2. Fix `cycle.cc` in two ways. First, as the chapter suggested, let the
   timer's callback capture a plain pointer to the session. Then
   suppose that the callback might run after the session was dropped,
   and give it a pointer that doesn't keep the session alive: Seastar's
   `weak_ptr`, from `<seastar/core/weak_ptr.hh>`. Check both with
   LeakSanitizer.

3. Fix `stuck.cc`'s bug: a request should wait for its reply for a
   second at most, and give up. Run it again: do the live objects stop
   growing? How many rounds does it take?

4. Add to the repeat test's workload two more ways in which a client
   abandons its work: a client that sends half a request and hangs up,
   and one that sends a pipeline of many requests and hangs up before it
   reads the replies. Does Shoal 16 leak with them?
