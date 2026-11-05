# Coordination {#ch:coordination}

Programming a Seastar shard is, in some ways, easier than programming
threads that share memory. A shard runs one task at a time, and never
interrupts it ([chapter](#ch:how-seastar-runs-your-code)), so between two
`co_await`s, a coroutine sees the shard's data as if nothing else
existed: there are no data races, so a shard's data needs no locks.
Shards don't share memory either, as [chapter](#ch:case-for-seastar)
explained: between them, there are only messages
([chapter](#ch:shared-nothing)). So a Seastar program's own code needs
none of the hardest parts of multithreaded programming: atomic
operations, lock-free algorithms, and the rules about the order in which
one core sees another core's writes (the "memory model" that
`std::memory_order` expresses).

But coordination is still needed, for two reasons. First, a coroutine's
work often spans several `co_await`s, and at each of them, other code may
run and change what the coroutine saw. Second, work must often be
limited: how many operations run at once, how much memory they use, how
much waits in queues. This chapter covers Seastar's tools for both. The
semaphore does most of the work. Then come the condition variable, the
lock for readers and writers, the gate (which keeps count of work running
in the background), and the queue.

Shoal's version 6 uses them all. It adds Redis's lists, whose command
`BLPOP` waits until a list has an element. It limits the number of
clients and the memory of their requests. And it delivers Pub/Sub
messages in a better way than [chapter](#ch:concurrency-within-a-shard)'s
version did.

## Why a shard still needs coordination

Here is a function that withdraws money from an account, if the balance
allows it. Between checking the balance and taking the money, it writes
a record of the withdrawal, which takes a moment:

```{.cpp include="examples/coordination/withdraw.cc" snippet="withdraw"}
```

Each line of this function runs without interruption, and yet it is
wrong. While one call waits in `log_withdrawal()`, another call can run,
check the same balance, and see that the money is still there. The
program `withdraw.cc` runs two withdrawals of 80 from a balance of 100 at
the same time, and prints:

```
withdraw: 2 of 2 withdrawals done, balance -60
```

This is the same mistake as the second attempt at `KEYS` in
[chapter](#ch:how-seastar-runs-your-code), in another form: a coroutine
looked at something before a `co_await`, and relied on it after the
`co_await`. Check, wait, then act: whenever a coroutine's steps follow
this pattern, the check may no longer be true when it acts. The cure is
to make sure that nobody else does the same steps at the same time:

```{.cpp include="examples/coordination/withdraw.cc" snippet="withdraw-safely"}
```

and now one of the two withdrawals is refused:

```
withdraw_safely: 1 of 2 withdrawals done, balance 20
```

## Semaphores {index="`seastar::semaphore`"}

### A count of units

A Seastar [*semaphore*]{index="`seastar::semaphore`" .main}, from
`<seastar/core/semaphore.hh>`, is a counter of units. Taking units waits until the counter has enough of them, and
giving them back lets those who wait proceed. In `withdraw_safely()`, the
counter starts with one unit, so only one withdrawal at a time can hold
it: that is a [*mutex*]{index="mutex" .main}, a lock that lets only one
holder in, which is the special case of a semaphore with one unit.
Seastar has no separate class for it. (Redpanda wrote its own small
`ssx::mutex`, a semaphore of one unit with a more fitting name.)

[`get_units()`]{index="`seastar::get_units()`" .main} takes units, and
returns a [`semaphore_units`]{index="`seastar::semaphore_units`"} object,
which gives those units back when it's destroyed. `held`, in
`withdraw_safely()`, is destroyed when the coroutine ends, however it
ends: when it returns `false`, `true`, or with an exception. Another
function, [`with_semaphore()`]{index="`seastar::with_semaphore()`"}, is
for protecting a single function call: it takes the units, calls the
function, and gives the units back when the function's future is
available.

`semaphore_units` is a template, whose parameters are those of its
semaphore's type, with defaults that fit a plain `semaphore`: so the
units of a `semaphore` are a `semaphore_units<>`, as in Shoal's code
later in this chapter.

> **C++ corner: why `std::string` is an alias.**[]{index="C++ corner!why `std::string` is an alias"}[]{index="`std::basic_string`"}
> Code must write `semaphore_units<>`, not just `semaphore_units`,
> even when every template argument is the default. The standard library
> avoided this problem long ago, in `std::string`, where code can write
> `std::string` and not `std::string<>`, as it would if the template
> were called `std::string`, with `char` as its default type of
> characters. The library achieved this
> by calling its string template `std::basic_string`. That template has
> three parameters: the type of the characters, and two with defaults,
> the character traits, which say how to compare and copy characters,
> and the allocator. `std::string` is only an alias,
> `using string = basic_string<char>;`. Seastar's
> semaphore is built the same way: the template is
> [`basic_semaphore`]{index="`seastar::basic_semaphore`"}, whose
> parameters choose the exceptions that a failed wait throws, and the
> clock that measures its timeouts, and `semaphore` and
> `named_semaphore` are aliases.
>
> Unfortunately, `semaphore_units` didn't use this technique, so code
> must write `semaphore_units<>` for the units of a plain `semaphore`.
> It's too late to change that: as [chapter](#ch:getting-started)
> explained, Seastar promises that an
> application that compiled keeps compiling, and an alias that isn't a
> template can't be written with `<>`, so every `semaphore_units<>` in
> existing applications would break.
>
> A better idea, not yet in Seastar, could be an alias inside
> `basic_semaphore`, `using units = semaphore_units<ExceptionFactory, Clock>;`,
> which knows its semaphore's parameters, so code could write
> `semaphore::units`, or `named_semaphore::units`, and never think about
> them.
>
> Even now, a variable with an initializer can leave out the `<>`: since
> C++17, in `semaphore_units held = co_await get_units(sem, 1);`, the
> compiler deduces the arguments from the initializer. But a member, a
> parameter or a return type has nothing to deduce them from.

The semaphore also has methods that take and give back units directly,
`wait()` and `signal()`. They are error-prone: code that uses them must
call `signal()` on every path out of the function, including every
exception, and it's easy to miss one. Avoid them when you can, and use
`get_units()` or `with_semaphore()`, which give the units back on every
path.

A semaphore with more units limits how many operations run at once. Here,
at most three downloads run together:

```{.cpp include="examples/coordination/units.cc" snippet="concurrency"}
```

Ten calls of `download()`, all started at once by
`coroutine::parallel_for_each()` from
[chapter](#ch:concurrency-within-a-shard), run three at a time, and the
others wait for their turn. (That chapter's `max_concurrent_for_each()`
does the same for a loop; a semaphore works for any code, even code that
isn't a loop, and can be shared by several loops.)

But the units needn't be operations: they can count anything, and code
can take many units at once. For example, a semaphore can count bytes of memory: each
request takes as many units as the bytes it will use, so that the memory
of all the requests together stays within a limit:

```{.cpp include="examples/coordination/units.cc" snippet="bytes"}
```

This prints:

```
40 left, 2 requests waiting
40 left, 0 requests waiting
```

Waiting requests are served in the order in which they came. The request
for 10 waits, although 10 of the 40 units are available, because the request
for 50 came first. This is fair: if small requests could overtake a big
one, a steady flow of them could make it wait forever. It also means that
a big request at the head of the line holds up everything behind it.
When the first request gives back its 60 units, both waiting requests are
served at once, and 40 units are left. For the same reason of fairness,
[`try_get_units()`]{index="`seastar::try_get_units()`"}, which takes units
only if it can without waiting, fails when others are already waiting,
even if there are enough units.

The example also uses two more of the semaphore's methods:
[`available_units()`]{index="`seastar::semaphore`!`available_units()`"}
returns how many units are available, and
[`waiters()`]{index="`seastar::semaphore`!`waiters()`"} how many requests
wait for units. And it calls
[`return_all()`]{index="`seastar::semaphore_units`!`return_all()`"} on
`first`, a `semaphore_units` object, to give back all the units that
`first` holds before `first` is destroyed. A `semaphore_units` object can
also give back only some of its units, with
[`return_units()`]{index="`seastar::semaphore_units`!`return_units()`"},
which takes a number of units. And
[`split()`]{index="`seastar::semaphore_units`!`split()`"} moves some of
its units to a new `semaphore_units` object, which can then be handed to
other code;
[`adopt()`]{index="`seastar::semaphore_units`!`adopt()`"} merges them back.

Sometimes code must take units without waiting, for example because the
memory they stand for is already in use.
[`consume_units()`]{index="`seastar::consume_units()`"} takes them without
waiting, even if the counter goes below zero; then others wait until
enough units are given back to bring it above their needs. Shoal uses it
below.

### Ending a wait early

A request for more units than the semaphore will ever have waits
forever, and so does a request whose units others never give back. With
a timeout, such a wait fails when the timeout expires, instead of waiting
forever:

```{.cpp include="examples/coordination/units.cc" snippet="too-many"}
```

```
Semaphore timed out: memory
```

The request leaves the line of waiters, as [chapter](#ch:time)
promised, and fails with a
[`semaphore_timed_out`]{index="`seastar::semaphore_timed_out`"}. The
semaphore here is a
[`named_semaphore`]{index="`seastar::named_semaphore`" .main}, which adds
its name to the messages of its exceptions. A plain `semaphore`'s message
is only "Semaphore timedout", which isn't much help in a server with
hundreds of semaphores.

A wait can also end with an
[`abort_source`]{index="`seastar::abort_source`"}, from
[chapter](#ch:time): `get_units(sem, n, as)` fails with a
[`semaphore_aborted`]{index="`seastar::semaphore_aborted`"} when an abort
is requested through `as`.

The semaphore's
[`broken()`]{index="`seastar::semaphore`!`broken()`"} method makes every
waiter fail at once, with a
[`broken_semaphore`]{index="`seastar::broken_semaphore`"}, and every
later wait too: that's how code that is shutting down tells all the
fibers waiting for a resource that it won't come.

### What a semaphore costs

A Seastar semaphore is not a lock of threads, such as `std::mutex`. When
a coroutine waits for units, the coroutine is suspended, and the thread
goes on running other tasks. No atomic instruction is needed, because
the semaphore belongs to one shard, and only its thread touches it. So a
semaphore costs little. On the laptop of
[chapter](#ch:tutorial-introduction), the program `semaphore-cost.cc`
measured:

| What | Cost |
|---|---|
| `get_units()` and giving the unit back, when it's available | 4.1 ns |
| `try_get_units()` and giving it back, when it's available | 1.0 ns |
| `std::mutex`, locked and unlocked on one core, when it's available | 12.5 ns |
| a unit passed back and forth between two fibers, per turn | 56 ns |

`try_get_units()` is cheaper than `get_units()` because it returns the
units directly, in a `std::optional`, while `get_units()` returns them in a future, which the
coroutine must `co_await`. That costs a few nanoseconds even when the
future is ready, as the sidebar below explains. But `try_get_units()`
can't wait, so it suits only code that has something else to do when the
units aren't available.

> **Why `co_await` of a ready future isn't free.** A function that
> returns a value just returns it. A coroutine that `co_await`s a future
> that is already available does more, though it never suspends:
>
> - The future, with the value inside it, is built in the coroutine's
>   frame.
> - `co_await` checks that the future is available, and calls
>   `need_preempt()` ([chapter](#ch:how-seastar-runs-your-code)). That
>   reads two numbers from memory every time: the kernel can change them
>   at any moment, so the compiler can't keep them in a register, and
>   Seastar adds a compiler fence to make sure that the compiler doesn't
>   hoist the reads out of a loop.
> - The coroutine records in its frame where to continue, in case it
>   must suspend after all.
> - It moves the value out of the future, checks that the future holds
>   no exception, and destroys the empty future.
>
> The coroutine's own variables that live across a `co_await`, such as a
> loop's counter, also stay in its frame, in memory, since they must
> survive if the coroutine suspends and continues later, from a
> different call. In one loop that we disassembled, GCC loaded the
> loop's counter from the frame, and stored it back, on every iteration.
> Each of these steps is cheap, but together they add up: in a separate
> measurement, a `co_await` of a ready `future<int>` added about 1.3
> nanoseconds to each iteration of a loop.

The last line is the cost when the semaphore actually makes someone wait:
a fiber takes the unit, yields, and gives it back, while another fiber
waits for it. Each turn suspends two coroutines, the one that yields and
the one that waits, and [chapter](#ch:futures-and-promises) measured
about 25 nanoseconds for a coroutine that waits once, so most of the cost
is the waiting, not the semaphore.

### Deadlocks

Like a lock of threads, a semaphore can deadlock. Here, a function that
holds the table's lock calls another function that takes the same lock:

```{.cpp include="examples/coordination/deadlock.cc" snippet="deadlock"}
```

`update_index()` waits for the unit that its caller holds, and its caller
waits for `update_index()`: neither will ever proceed. A thread that
deadlocks at least stops, and a debugger shows where. Here, nothing
stops: the shard goes on running everything else, and the two coroutines
just never end. The stall detector of
[chapter](#ch:how-seastar-runs-your-code) sees nothing, because no task
runs for long; the program `deadlock.cc` runs a heartbeat next to the
deadlock, and it beats 20 times in the 200 milliseconds before the
timeout:

```
update_index: Semaphore timedout
meanwhile, the heartbeat beat 20 times
```

Without the timeout, only the requests that never get a reply would tell
that something is wrong. The usual rules for locks apply: never wait for
units that you already hold, and when you need several semaphores, always
take them in the same order. And a timeout, even a long one, on waits
that should never take long turns a silent deadlock into an error that
someone will see.

## Condition variables {index="`seastar::condition_variable`"}

A semaphore waits for units. Sometimes a coroutine must wait for
something else: until a queue has an item, or until a background job
finishes. A
[*condition variable*]{index="`seastar::condition_variable`" .main},
from `<seastar/core/condition-variable.hh>`, lets code that changes the state wake up the coroutines that wait for it to
change. Here, `take()` waits until a deque has an item:

```{.cpp include="examples/coordination/condition.cc" snippet="items"}
```

[`signal()`]{index="`seastar::condition_variable`!`signal()`"} wakes up
the first waiter, and `broadcast()` all of them. `wait()` can also be
given a timeout, after which it fails with a
`condition_variable_timed_out`, and the condition variable's `broken()`
makes all its waiters fail, as the semaphore's does. Why `take()` checks
the deque again after every wait, in a loop, is the second of the
differences below.

Seastar's condition variable differs from `std::condition_variable` in
two ways.

The first difference: a signal that finds nobody waiting isn't lost.
The condition variable remembers it, and the next `wait()` consumes it,
and returns at once. When the waiter checks the state before it waits,
as `take()` does, this changes nothing, because the waiter sees the
change itself. At most, a remembered signal wakes up a later wait for
nothing, and the check sends the waiter back to waiting. The remembered
signal helps a loop that doesn't check the state before it waits, and
waits only for news that something changed, such as a fiber that saves
the state in the background after each change:

```{.cpp include="examples/coordination/condition.cc" snippet="remembered"}
```

A change made while the saver is saving finds nobody waiting. A
`std::condition_variable` would drop its signal, and the change would
stay unsaved until the next change. Here, the next `wait()` returns at
once, and the saver saves again.

The condition variable remembers only one signal, not a count of them:
two changes during a save cause one more save, not two. For a
loop like this one, that is exactly right, because one save writes all
the changes made so far, and a count would only add saves of the same
state. When each signal stands for one thing to handle, such as an item
or a request, use a semaphore, which counts: each `signal()` adds a
unit, and each `wait()` takes one.

The second difference matters more. With `std::condition_variable`, a
waiter wakes up holding the mutex, so no other thread can change the
state between the waiter's check of it and what the waiter does next.
Seastar's condition variable has no mutex, and waking up doesn't run the
waiter immediately. It only puts the waiter in the queue of tasks, like
everything else that a future makes ready, and until the waiter's turn
comes, other code runs, and may change the state again. A waiter that
waits only once, and doesn't check again, shows it:

```{.cpp include="examples/coordination/condition.cc" snippet="take-once"}
```

Here, the code that adds an item takes it right back, before the waiter
runs:

```{.cpp include="examples/coordination/condition.cc" snippet="meanwhile"}
```

This prints

```
took 1 first, and the waiter got -1
```

The waiter was woken up when there was an item, and when it ran, the
item was gone. So a waiter must check the condition again after it wakes
up, and wait again if it's false, as `take()`, at the start of this
section, does. With `take()` instead of `take_once()`, the same code
prints

```
took 1 first, and the waiter got 2
```

The loop is safe because, as [chapter](#ch:how-seastar-runs-your-code)
explained, a shard runs one task at a time, and never interrupts a task.
So between two `co_await`s, a coroutine's code runs as if nothing else
existed. `take()` checks the deque, and takes the item, with no
`co_await` in between, so the item it found is still there when it takes
it.

Seastar also has waits that take a predicate, and do this loop for
you: `wait()` with a predicate, such as
`co_await item_added.wait([] { return !items.empty(); })`, and
[`when()`]{index="`seastar::condition_variable`!`when()`"}, a form of
waiting that works only with `co_await`:

```{.cpp include="examples/coordination/condition.cc" snippet="take-when"}
```

Both do the loop, but it isn't always enough. Each checks the predicate
in one task, and the waiting coroutine continues in a later one, and
other code may run in between. `when()` checks the predicate early, when the signal
comes, inside `signal()`. So with `take_when()`, the same code prints

```
took 1 first, and the waiter got -1
```

The waiter was woken up, because there was an item at the time of the
signal, and found none when it ran. (Our first version of this example
didn't check `items.empty()`, and read the front of an empty deque.)
`wait()` with a predicate checks it later, in a task of its own, after
the waiter wakes up. In the same code, it found no item, waited again, and
got item 2. But the gap is still there, only shorter: we tried it with a
task queued just after the waiter's wakeup, which took the item, and the
waiter of `wait()` also continued with the deque empty. Nothing can
close this gap, because nothing knows when the waiting coroutine will
run. So when a wait with a predicate ends, the predicate was true at some
moment after the wait began, but may no longer be.

If the predicate is *monotonic*, once true, it stays true, such as a
flag that says that something finished, then nothing can make it false
again, and the waiter can rely on it, and `wait()` or `when()` with a
predicate becomes useful. Most predicates are like that. But a waiter
whose condition other code can make false again, as in our example, must check it
again after the wait, in a loop like `take()`'s, and shouldn't use
`wait()` or `when()` with a predicate at all.

And whichever way the waiter checks, it lost its turn: the item went to
someone who didn't wait at all. Whether that matters depends on the
program. In Shoal's version 6, at the end of this chapter, it matters:
Redis promises that an element pushed to a list goes to a client that is
already waiting for it, in the command `BLPOP`, and not to a client that
asks for it later. A condition variable can't keep that promise.

Why does Seastar have both `wait()` and `when()`? `wait()` is the
original, and returns a future, so it works anywhere: in a coroutine, in
a chain of continuations, and in a `seastar::thread`. `when()` was added
later, for coroutines only. It keeps its waiter in the coroutine's frame,
while `wait()` allocates one on the heap for every wait that doesn't
return at once. So in a coroutine, `when()` is the better choice, and
`take()` could just as well wait with `co_await item_added.when()`.
Without a predicate, the two behave the same. With one, they check it at
different times, as we saw, but neither saves the loop when the
predicate isn't monotonic.

## Read-write locks {index="`seastar::rwlock`"}

Many operations only read a data structure, and may safely do it
together; an operation that changes it must do it alone. A
[*read-write lock*]{index="read-write lock" .main} lets any number of
readers hold it together, or one writer. Seastar's is `rwlock`, from
`<seastar/core/rwlock.hh>`:

```{.cpp include="examples/coordination/rwlock.cc" snippet="rwlock"}
```

This prints

```
reader 1 reads
reader 2 reads
the writer writes
reader 3 reads
```

The first two readers read together. The writer waits for them, and the
third reader, which came after the writer, waits for the writer, although
it could read with the first two: otherwise, a steady flow of readers
could keep a writer waiting forever. Seastar's `rwlock` is built on a
semaphore that starts with the largest count a semaphore can have,
2^63^ − 1 units (the largest `ssize_t`), far more than there can ever be
readers. A reader takes one unit,
and a writer takes them all, and the semaphore's fair order does the
rest. `hold_read_lock()` and `hold_write_lock()` return objects that give
the lock back when destroyed, like `get_units()`.

Seastar also has a [`shared_mutex`]{index="`seastar::shared_mutex`"}, from
`<seastar/core/shared_mutex.hh>`, with
the same purpose and an interface like `std::shared_mutex`: `lock()` and
`lock_shared()`, used with `with_lock()` and `with_shared()`, or with
`get_unique_lock()` and `get_shared_lock()`, which return a standard
lock, a `std::unique_lock` or a `std::shared_lock`.

`rwlock` and `shared_mutex` are two implementations of the same lock,
with different interfaces. Both serve their waiters in order, and in
both, a writer that waits makes the readers that come after it wait too.
So use whichever interface you find more convenient, with one
exception: only `rwlock` can wait with a timeout or an abort source.
Its `read_lock()`, `write_lock()`, `hold_read_lock()`, and
`hold_write_lock()` take either one, as the semaphore's `wait()` does.

## Gates {index="`seastar::gate`"}

[Chapter](#ch:concurrency-within-a-shard) left a problem open: nothing
waits for work that runs in the background. An object that such work
uses can't be destroyed until the work ends, and
[chapter](#ch:object-lifetime) promised a tool that counts the work, and
lets the owner wait for all of it. That tool is the
[*gate*]{index="`seastar::gate`" .main}, from `<seastar/core/gate.hh>`:

```{.cpp include="examples/coordination/gate.cc" snippet="gate"}
```

`hold()` returns a
[`gate::holder`]{index="`seastar::gate::holder`"}, and while it exists,
the gate counts it. Passed by value to the background coroutine, it lives
in the coroutine's frame until the coroutine ends.
[`close()`]{index="`seastar::gate`!`close()`"} waits until no holders are
left, and from then on, `hold()` refuses, with a
[`gate_closed_exception`]{index="`seastar::gate_closed_exception`"}:

```
3 saves in the background
saved file 1
saved file 2
saved file 3
closed, 0 in the background
too late: gate closed
```

So the owner of the gate closes it, and waits, before destroying what the
background work uses, and the work that comes too late is refused rather
than left using destroyed objects. `try_hold()` returns an empty
`std::optional` instead of throwing, `is_closed()` tells whether the
gate was closed, and `check()` throws if it was, for long background work
that should stop early when its owner is going away. And `with_gate(g, f)`
runs `f`, holding the gate until its future is available. Gates are
common in both large projects. Redpanda even wrote its own
`ssx::spawn_with_gate()`, which starts background work holding a gate,
and hides the exceptions that a closing gate causes. [Chapter](#ch:clean-shutdown) uses a gate to shut
Shoal down cleanly, and a `named_gate`, like a `named_semaphore`, names
itself in its exception.

## Queues {index="`seastar::queue`"}

A [`queue`]{index="`seastar::queue`" .main}, from `<seastar/core/queue.hh>`, passes items from a producer
to a consumer, and makes each side wait for the other: a consumer waits
while the queue is empty, and a producer while it's full. The queue's
size is limited: its constructor takes the most items it may hold, and
`set_max_size()` can change that limit later. The limit is what keeps a
fast producer from running ahead of a slow consumer:

```{.cpp include="examples/coordination/queue.cc" snippet="queue"}
```

With a queue of at most three items, created as
`queue<std::optional<int>> q(3)`, the producer of `queue.cc` waits for the
consumer, which needs a millisecond for each item, and at most three
items wait at a time. An item that waits costs only its place in the
queue's array, and not a coroutine frame and a future of its own, as a
background write did in [chapter](#ch:concurrency-within-a-shard).

A queue is for one producer and one consumer. That means that while one
fiber waits in `push_eventually()`, no other fiber may call
`push_eventually()`, and the same for `pop_eventually()`. The queue
keeps only one waiting producer and one waiting consumer: a second
fiber that waits replaces the first, whose wait fails with a
`broken_promise` exception. We tried it with two consumers waiting
together: the second got the item, and the first failed. Several fibers
may push to the same queue only if they take turns, so that no two of
them wait at the same time.

The queue has an `abort()`, which makes waiting producers and consumers
fail with an exception, and our first version of this example used it to
tell the consumer that the items were over. The consumer got only seven
of the ten items: `abort()` also destroys the items that are still in the
queue. It's for errors. The end of the data is better told by the data,
here with an empty `std::optional`.

A [`pipe<T>`]{index="`seastar::pipe`" .main}, from `<seastar/core/pipe.hh>`, is a
wrapper of a queue, which adds an end for each side, a `pipe_writer` and
a `pipe_reader`, which each side takes. Each end
tells the other when it's gone, so the producer needn't send an empty
`std::optional` at the end: when the writer is destroyed, the reader's
`read()`, after the items left, returns one, and when the reader is
destroyed, the writer's next `write()` fails with
`broken_pipe_exception`, instead of waiting for a consumer that will
never come.

Neither ScyllaDB nor Redpanda uses `pipe`. Its two ends fit a producer
and a consumer that are each a fiber of its own, which owns its end and
destroys it when it finishes. In a server, the two sides of a queue are
usually parts of long-lived objects instead, which stop when the server
stops. For them, separate ends make stopping harder, not easier. A pipe
has no `close()`, since an end is closed only by being destroyed, so
stopping means destroying an end at the right moment. And the pipe hides
its queue's `abort()`, which is how a stopping server makes a waiting
producer or consumer fail. For such servers, a `queue`, with `abort()`,
fits better.

## Shoal, version 6 {index="Shoal!version 6"}

Version 6 adds three things to Shoal, each of which shows some of this
chapter's tools at work.[^shoal-v6] The first is Redis's lists, and with
them `BLPOP`, a command that waits until another client pushes an
element. It's built without a condition variable: the obvious version,
with one, breaks a promise that Redis makes, which shows in a real
program where a condition variable falls short, and what to use
instead. The second is limits, on the number of clients and on the
memory of the requests being read, each kept with a semaphore. The
third is a new way of delivering Pub/Sub messages, which replaces the
chain of writes of [chapter](#ch:concurrency-within-a-shard) with a
queue for each subscriber, and makes publishers wait, on a semaphore,
when subscribers fall behind.

[^shoal-v6]: The source of this version is in `shoal/v6/`.

### Lists {index="Redis commands!`LPUSH`"}

A Redis key can hold a list of strings instead of a single string.
`RPUSH key element...` adds elements at the end of the list, and `LPUSH`
at its start. `LPOP` and `RPOP` remove an element from either end.
`LLEN` returns the list's length, and `LRANGE key start stop` its
elements from `start` to `stop`. A list that becomes empty is deleted,
so a list that exists is never empty, and Shoal's code relies on that:
it takes the first element of a list it finds without checking. A
command for strings applied to a list, or a list command to a
string, fails with Redis's `WRONGTYPE` error. So a key's value is now
one or the other:

```{.cpp include="shoal/v6/commands.hh" snippet="entry"}
```

So `GET`, for example, now checks that it got a string:

```{.cpp include="shoal/v6/commands.cc" snippet="get"}
```

and the list commands find a key's list with `lookup_list()`, which also
tells them when the key holds a string:

```{.cpp include="shoal/v6/commands.cc" snippet="list-lookup"}
```

`push()` and `pop()` do the work of the four commands that change a list:

```{.cpp include="shoal/v6/commands.cc" snippet="push"}
```

(`LLEN` and `LRANGE`, which aren't shown, use `lookup_list()` too.) `push()` ends
with `serve_blocked()`, which is the subject of the next section. The
table of commands has the new ones:

```{.cpp include="shoal/v6/commands.cc" snippet="table"}
```

### `BLPOP` {index="Redis commands!`BLPOP`"}

`BLPOP key... timeout` pops the first element of the first of its lists
that isn't empty, like `LPOP`. But if they are all empty, it waits until
another client pushes an element to one of them, or until the timeout
passes, and then replies with nil. A timeout of 0 waits forever. With it,
a list becomes a queue of work between Redis clients: producers push, and
consumers wait in `BLPOP`. Redis promises a few things about the wait,
and Shoal's tests check them against Valkey:

- Clients that wait for the same key get its elements in the order in
  which they started to wait.
- A pushed element goes to a waiting client at once. Another client
  can't take it first, not even with an `LPOP` sent right after the push.
- A client that hangs up while it waits stops waiting, so that the next
  element pushed isn't given to a client that is gone.

The obvious way to write `BLPOP` is with a condition variable for each
key, which `push()` signals, and for which `BLPOP` waits with a
predicate:

```cpp
    auto& pushed = db.pushed[key];  // a condition_variable for each key
    co_await pushed.wait(timeout, [&] { return has_element(db, key); });
    // ...pop the first element, and reply with it
```

We tried it. It passes most of the tests, but not the second promise:
after a client's `RPUSH` signals the waiter, the same client's `LPOP`
runs before the waiter's turn comes, and takes the element. The waiter
wakes up, finds the list empty, and waits again, until its timeout. This
is the lesson of the condition variable above, in a real program: waking
a waiter only puts it in the queue. Nor does this version notice a client
that hangs up, so the next push hands its element to a dead connection,
and the element is lost.

So version 6 doesn't wake waiters to look for themselves: `push()` gives
them their elements. Each waiting client is a `blocked_pop`, which waits
in the line of every key it was given:

```{.cpp include="shoal/v6/commands.hh" snippet="blocked-pop"}
```

and the store keeps these lines, one for each key that has waiters:

```{.cpp include="shoal/v6/commands.hh" snippet="store"}
```

After a push, `serve_blocked()` pops elements for the waiters, in their
order, while the list has elements, and sets each waiter's promise with
the element it got. The element is out of the list before any other
command can run, so nobody can take it first:

```{.cpp include="shoal/v6/commands.cc" snippet="serve-blocked"}
```

`end_wait()` takes the waiter out of the lines of all its keys, not only
the one whose element it got, so that it doesn't get another one later.
Here is `BLPOP` itself:

```{.cpp include="shoal/v6/commands.cc" snippet="blpop"}
```

(`parse_timeout()`, not shown, parses a number of seconds, which may
have a fraction.) Three things can end the wait, and whichever comes
first ends it, with `end_wait()`. A push gives an element. The timer
fires, if there is a timeout. And the connection's `hung_up` abort
source, which [chapter](#ch:time) showed, is aborted when the client
hangs up; its subscription calls the same function as the timer. As
[chapter](#ch:time) explained, if the abort was already requested,
`subscribe()` doesn't call the function, but returns an empty
subscription, which evaluates to `false`. So if the client hung up
before `BLPOP` subscribed, `blpop()` calls `give_up()` itself.
`b.waiting` makes sure that only the first of them counts: all three run
on the shard's one thread, so the first sets it, and the others see it.
The `blocked_pop`, the timer and the subscription are local variables,
in the coroutine's frame, so they live while the coroutine waits; when
it resumes and ends, the timer is cancelled and the subscription
removed, by their destructors.

How does Shoal know that a client hung up, while its connection's fiber
is waiting in `BLPOP`, and not reading from the client? Seastar's
[`wait_input_shutdown()`]{index="`seastar::connected_socket`!`wait_input_shutdown()`"}
returns a future that becomes available when the client closes its side
of the connection, even if nobody reads from it. `handle_connection()`
calls it once, when the connection starts, and triggers `hung_up` when
it's available, with `then()`. As [chapter](#ch:object-lifetime)
explained, `then()` is a method of every future, which calls a function
with the future's value once it's available, and
[chapter](#ch:continuations) explains it fully:

```{.cpp include="shoal/v6/main.cc" snippet="handle-connection"}
```

The future also becomes available when our own side calls
`shutdown_input()`, so at the end of the connection, `handle_connection()`
does that, and waits for the future, which can't be left behind: its
continuation uses `hung_up`, a local variable.

### Limits {index="admission control"}

A server that accepts any amount of work runs out of something sooner or
later. Version 6 limits two things, each with a semaphore, which the
connections share:

```{.cpp include="shoal/v6/main.cc" snippet="limits"}
```

The first limit is on the number of clients, as Redis's `maxclients`
does, 10,000 by default. Each connection holds a unit of the semaphore `clients`, and
when none is left, Shoal refuses the connection with Redis's error:

```{.cpp include="shoal/v6/main.cc" snippet="refuse"}
```

The accept loop takes the unit with `try_get_units()`, since it mustn't
wait (a client that can't be served should be told so at once), and
gives the units to `handle_connection()` as a parameter, so that they
live in its frame, and are given back when the connection ends:

```{.cpp include="shoal/v6/main.cc" snippet="shoal-main"}
```

The second limit is on the memory of the requests being read. A Redis request
announces the length of each argument before sending it, and Shoal's
request reader allocated the memory for an argument as soon as it read
its length, up to Redis's limit of 512 MB for an argument. So ten clients
that each announce a value of 60 MB make Shoal allocate 600 MB at once.
Shoal's tests run it with half a gigabyte for each shard, and the new test
`test_many_large_values` does exactly that: with version 5, seven of the
ten connections failed with `std::bad_alloc`. A server can't let its
clients decide how much memory it allocates.

So version 6 admits large requests, those whose arguments add up to more
than 64 KB, before it reads the argument that brings them over 64 KB: their
memory is taken, in units of bytes, from a semaphore that allows a
quarter of the shard's memory (`--max-request-memory` changes that), and
given back when the request has been executed.

Version 5 had a second hole of the same kind. Its request reader, which
[chapter](#ch:coroutines) showed, called `reserve()` on the request's
vector as soon as it read the number of arguments, which a client may
set to a million, and then send none: room for a million `sstring`s,
32 MB, allocated for nothing. Version 6 doesn't call `reserve()`, and
lets the vector grow as the arguments arrive.

```{.cpp include="shoal/v6/resp.hh" snippet="reader"}
```

```{.cpp include="shoal/v6/resp.cc" snippet="admit"}
```

A client whose argument must wait doesn't get its data read: it waits in
the kernel's buffers, and when they are full, TCP stops the client from
sending more, so the wait reaches all the way back to the client.

Only a request's first admission waits, while the request holds no
units yet. A request that waited while holding units could deadlock: if
every request in progress held some units and waited for more, nobody
would ever give any back. So a later admission takes its units only if
they're free, and otherwise rejects the request. `read()` then gives
back the request's units and the arguments it has read, reads the rest
of the request without keeping it, and fails with `request_too_large`,
which `serve()` sends to the client, as the error `not enough memory for
the request`, before it closes the connection. Reading to the end of
the request matters: when a server closes a connection whose client is
still sending, the kernel resets the connection, and the client may
never see the error. A request that needs more units than the
semaphore has could never get them, so it's rejected at once.

The price is that a request may fail that would have succeeded had it
waited. With the 128 MB that the tests' Shoal allows, when three clients
each push a hundred values of 1 MB at once, one succeeds, and two get
the error. A server that knows a request's size before it reads the
request can wait once for all of its memory, and never reject it.
ScyllaDB's CQL protocol allows that: every request begins with its
length, and ScyllaDB's server waits for that much memory, and some
more, before it reads the rest. A Redis request doesn't say its size,
and Redis protects its memory by disconnecting clients: a client whose
unread requests reach 1 GB, and, with the option `maxmemory-clients`,
the clients that use the most memory, when all of them together use
too much.

With these limits, `test_many_large_values` passes: Shoal reads the
values a few at a time, and none of the ten requests fails. A second
new test, `test_many_large_lists`, gives Shoal
32 MB for requests, and sends 32 lists of 24 values of 1 MB at once.
When a later admission took its units without waiting, even beyond
the limit, as we first wrote it, 14 of the 32 connections failed with
`std::bad_alloc`. Now one request succeeds, and the others get the
error. The other
side of admission control is what happens when the memory is used up
anyway. In Seastar, that is worse than in an ordinary program, as
[chapter](#ch:memory) explains: memory may run out inside Seastar's own
code, where nothing can recover, so a Seastar server must not count on
handling `std::bad_alloc`, but must bound the memory that its work can
use, as Shoal does here.

### Pub/Sub, revisited {index="Pub/Sub"}

[Chapter](#ch:concurrency-within-a-shard)'s Pub/Sub chained each write to
a subscriber to the previous one, with a `shared_future`, and each
message that waited cost a coroutine frame, a future and their
allocations. And when all the subscribers fell behind, it could only
disconnect them. Version 6 replaces the chain with a queue, and one
fiber for each client, which writes what the queue holds:

```{.cpp include="shoal/v6/pubsub.hh" snippet="client"}
```

```{.cpp include="shoal/v6/pubsub.cc" snippet="send"}
```

`send()` doesn't wait: the queue has no limit on its length, because the
limit on memory, below, is what counts. The writer waits until the queue
isn't empty, and then writes everything that waits in it before a single
flush, so that a burst of messages goes out together. A message carries the units it
took from the semaphore of Pub/Sub's memory, in a `semaphore_units`
object, so they are given back when the message is destroyed: after it's
written, or when `disconnect()` aborts the queue, which destroys the
messages in it. `drain()` waits with `when()`, here rightly: it waits
until everything sent before it was written, and if more messages are
sent after the signal and before `drain()` resumes, it needn't wait for
them too.

A client that isn't subscribed has no other writers, so its replies
don't go through the queue: `reply()` writes them directly to the
stream, as the replies to other commands are. `PUBLISH` first waits
for units, as much memory as the copies of the message to all the
subscribers will use:

```{.cpp include="shoal/v6/pubsub.cc" snippet="publish"}
```

While it waits, clients may subscribe or unsubscribe, so it looks at
the channel's subscribers again after the wait, and then sends to all of
them without waiting again, so that none of them can disconnect in the
middle. `split()` gives each subscriber's copy of the message its share
of the units. A single slow subscriber is still disconnected when its
messages use more than 32 MB, as Redis does, because otherwise one
client that never reads would make every publisher wait for it. But when
all the subscribers fall behind, the publishers now wait, and publish at
the pace at which the subscribers read.

How do the two versions compare? First, what a waiting message costs. As
in [chapter](#ch:concurrency-within-a-shard), we subscribed a client that
never reads, published 200,000 messages to it, and measured how much
Shoal's memory grew, for versions 4 and 6 on the same day:

| Message | Version 4 | Version 6 |
|---|---|---|
| 100 bytes | 713 bytes | 157 bytes |
| 1,000 bytes | 1,919 bytes | 1,321 bytes |

A message waiting in a queue costs its bytes, rounded up by the
allocator, and a little more; in the chain, it also cost a coroutine
frame and a `shared_future`. (`waiting_cost()`, in the `send` snippet,
counts what we measured.)

Second, what happens when subscribers can't keep up. Ten subscribers
read about 6 MB a second each, which is slow, but they do read, and a
publisher published 20,000 messages of 1,000 bytes as fast as it could,
with `--pubsub-max-total-pending 4000000`, a small limit. In three runs:

| | Version 4 | Version 6 |
|---|---|---|
| `PUBLISH`es per second | 507,000–592,000 | 6,283–6,294 |
| Subscribers disconnected | all 10 | none |
| Messages each subscriber got | 2,592–2,667 | all 20,000 |

Version 4 disconnected every subscriber once its messages went over the
limit. Version 6's publisher waited instead, and published at the pace at
which the subscribers read: 6,300 messages of a little over 1,000 bytes
a second is about the 6 MB a second that each subscriber reads.

And third, speed, when the subscribers keep up. With `pubsub-bench`, as
in [chapter](#ch:concurrency-within-a-shard) (a hundred subscribers, a million messages of 100 bytes,
1,024 `PUBLISH`es at a time), in three runs:

| Publishers | Version 4 | Version 6 |
|---|---|---|
| 1 | 11,290,000–11,360,000 | 10,270,000–10,620,000 |
| 2 | all 100 subscribers disconnected | 10,360,000–10,540,000 |

(messages delivered per second). Version 6 is slower when nothing waits,
by less than a tenth: waking the writer and passing through the queue costs a
little more than version 4's writes, which started inside `send()` when
nothing was ahead of them. Our first version of the queue was much
slower, 6,300,000–6,400,000 a second, and a profiler showed where the
time went. A tenth of it was `broadcast()` of the condition variable
after every write, even when nobody waited, and more was waking the
writer through `pop_eventually()`, which adds a continuation of its own.
Waiting with `not_empty()` instead, and signaling only when someone
waits, brought it to about 9,000,000. The rest was the publisher: its own
replies went through its queue too, so after every `PUBLISH` it waited
for its writer, and writing them directly, with `reply()`, brought it to
what the table shows. Most of what remains is copying the message for each of the
hundred subscribers, which version 4 does too.

## Exercises

1. Add Redis's `BRPOP`, which is `BLPOP` popping from the end of the list
   instead. Write its tests first, and check them against Valkey: in
   particular, what do a client blocked in `BRPOP` and one blocked in
   `BLPOP` on the same key get, when another client pushes three
   elements? What do `blocked_pop` and `serve_blocked()` need to know?

2. `admit()` lets only a request's first admission wait. Change it so
   that every admission waits for its units, and run Shoal with
   `--max-request-memory 104857600` (100 MB). Write a client that sends
   three requests at once, each an `RPUSH` of two elements of 40 MB, on
   connections of its own. What happens, and why? Which units does each
   request hold, and which does it wait for?

3. In `withdraw.cc`, replace the semaphore in `withdraw_safely()` with a
   `std::mutex`, held by a `std::lock_guard` across the `co_await`. What
   happens when you run it? Try to stop it with `kill`, which sends
   SIGTERM. Why doesn't that work either?

4. In `gate.cc`, let the gate be destroyed without `close()` while a save
   still holds it. What happens, in a `release` and in a `debug` build?
   Why is that better than letting the save go on?

5. Run Shoal with `--pubsub-max-total-pending 10000000`, subscribe three
   clients that never read, and publish messages of 100 KB to them. What
   happens to the publisher, and why aren't the subscribers disconnected?
   Give the publisher's wait for units a timeout of a second, after which
   it disconnects the subscriber that is furthest behind, and waits again.
   How long do a thousand `PUBLISH`es take now?
