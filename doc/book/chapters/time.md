# Time {#ch:time}

A server deals with time constantly. It gives up on a request that takes
too long, deletes data whose time has passed, does background work every
few seconds, and tells a client how long until something happens. This
chapter covers Seastar's tools for all of that: its clocks, which are
cheaper and coarser than the standard ones; timers, which call a
function at a given time; timeouts, which stop waiting; and abort
sources, which stop the work itself. It ends with a clock that moves
only when a test moves it, for testing code that waits.

Shoal's version 5 uses most of these. It adds Redis's expiration times:
a key can be given a time to live, after which it's deleted.

## Clocks {index="clocks"}

Seastar code uses several clocks, each with different properties. C++
provides two useful clocks:

- [`std::chrono::steady_clock`]{index="`std::chrono::steady_clock`"} is
  *steady*: it only moves forward, at a constant rate, whatever happens
  to the computer's date and time. Its time has no meaning by itself,
  but the difference between two of its times is a reliable duration, so
  it's the clock for measuring how long something took, and for deciding
  when to give up.
- `std::chrono::system_clock` is the computer's date and time, and is
  not steady: an administrator, or a time-synchronization daemon, can
  move it, backward or forward. It's for times that people see, or that
  must mean the same on another computer, not for durations.

Seastar adds two clocks of its own, from `<seastar/core/lowres_clock.hh>`,
which give the same two kinds of time, more cheaply: [`lowres_clock`]{index="`seastar::lowres_clock`" .main}, which is steady, and
[`lowres_system_clock`]{index="`seastar::lowres_system_clock`"}, which is the date and time. Reading them doesn't
ask the operating system: the reactor reads the real clocks from time to
time, and stores the result, and `now()` just reads the stored value.
Here is what reading each clock costs, on the laptop of
[chapter](#ch:tutorial-introduction):

| Clock | `now()` takes |
|---|---|
| `std::chrono::steady_clock` | 11.6 ns |
| `std::chrono::system_clock` | 11.3 ns |
| `lowres_clock` | 0.2 ns |
| `lowres_system_clock` | 0.2 ns |

Eleven nanoseconds is little, but a server that checks the time on every
request, as Shoal will, reads the clock millions of times a second, and
the low-resolution clocks make that free.

The downside of the lowres clocks is that, as their name
suggests, they have low resolution: the reactor updates the stored time
each time it goes around its loop, and at the start of each run of tasks, which
lasts at most a task quota ([chapter](#ch:how-seastar-runs-your-code)).
So while the shard is busy, the stored time may be up to a task quota
old: half a millisecond, unless `--task-quota-ms` changed it. That's fine
for timeouts and expiration times. But while a task runs, without
yielding, the low-resolution clocks stand still:

```{.cpp include="examples/time/clocks.cc" snippet="stale"}
```

This prints `in 5 ms of work, lowres_clock moved 0 ms`. So the
low-resolution clocks are for deciding *when* something should happen,
not for measuring how long a computation took: for that, use
`std::chrono::steady_clock`.

## Timers {index="`seastar::timer`"}

A [*timer*]{index="`seastar::timer`" .main} calls a function when a time comes. Seastar's
`timer<Clock>`, from `<seastar/core/timer.hh>`, measures the time with the clock it's given,
`std::chrono::steady_clock` by default, or `lowres_clock`, which is
cheaper for a timer whose time doesn't need to be precise. (Seastar's
headers write the default as `steady_clock_type`, which is only another
name for `std::chrono::steady_clock`.) The function is set when the
timer is constructed, or with `set_callback()`, and the timer is started
with one of:

- `arm(when)`, where `when` is a time, or a duration from now. Arming a
  timer that is already armed is a bug, which Seastar catches with an
  assertion.
- `rearm(when)`, which first cancels the timer if it's armed.
- `arm_periodic(period)`, which makes the timer call its function every
  `period`, until it's canceled.

`cancel()` stops a timer, and returns whether it was armed; `armed()`
tells whether it is, and `get_timeout()` when it will expire. Destroying a
timer cancels it. Here are two timers, one that ticks every 10
milliseconds, and one that stops it after 35:

```{.cpp include="examples/time/timers.cc" snippet="timers"}
```

The program prints:

```
tick 1
tick 2
tick 3
stopping the ticker; armed: true
3 ticks; armed: false
```

A timer's function is not a coroutine. It's an ordinary function, which
the reactor calls as a task of its own when the timer expires, so it must
be short, like any task, and it can't wait. A timer that should start
something that waits starts it in the background, with all that
[chapter](#ch:concurrency-within-a-shard) said about background work.
And a timer's function runs long after the code that armed the timer
has moved on, so whatever it uses must outlive the timer, the subject of
[chapter](#ch:object-lifetime). Here that's easy: the two timers, and the
variables their functions use, are local variables of a coroutine that
waits longer than both.

Most code doesn't use timers directly, but through functions built on
them. [`sleep()`]{index="`seastar::sleep()`"} is a timer whose function fulfills a promise, as
[chapter](#ch:futures-and-promises) showed by writing one, and the
timeouts and abort sources below use timers too.

### How timers work {index="`seastar::timer`!how it works"}

Seastar keeps the timers of each shard in a *timer set*, one for each
clock, designed so that arming and canceling a timer are cheap, however
many timers there are, and however far away they expire. A set has 64
buckets, each a linked list of timers ([Figure](#fig:timer-set)): one
for each of the 63 bits of a time, a signed 64-bit count of ticks, and
one for timers that are already due. A timer's bucket depends on the
highest bit in which its expiration time differs from the time the set
was last checked, so a timer that expires soon goes to one end of the
array, and a timer that expires in an hour to the other. The list's links
are in the timer object itself, so arming a timer doesn't allocate
memory: it computes the bucket, with a few instructions, and adds the
timer to its list. Canceling removes it from the list.

![A timer set, with 8-bit times instead of 64-bit ones. Each timer goes to
the bucket numbered by the position, counted from the left, of the
highest bit (underlined) in which its time differs from the time of the
last check, 80. So 200, which differs in the first bit, goes to bucket 0,
and 81, which differs only in the last, to bucket 7. Timers that are
already due go to a bucket of their own.](figures/timer-set.svg){#fig:timer-set}

When time passes, the reactor finds the bucket in which the current
time itself would go. Every timer in a nearer bucket is due, and every
timer in a farther one is not, so only the timers in the current time's
own bucket need checking one by one: those that are due run, and the
others go back into the set, into a nearer bucket, since their times now
differ from the time of the last check in a lower bit. In the figure,
if the time moves from 80 to 100, the timers for 81 to 95 are all due,
those for 128 and later are not, and only those for 96 to 127 are
checked. A timer that expires much later waits in a far bucket, and is
looked at only when the current time reaches its bucket: the timer for
200 stays in bucket 0 until the time passes 128, then moves to a nearer
bucket, and again when the time passes 192, and runs when it passes 200.
So a timer that expires in an hour moves a few times on its way, each
time with a list operation. That's more work than a timer that expires
in a millisecond needs, but not much more: every move takes the timer to
a nearer bucket, so it moves at most once for each bit of its time, and
none of this depends on how many other timers there are.

The reactor must also wake up in time, even when it has nothing else to
do. For `std::chrono::steady_clock` timers, it keeps one timer of the operating
system set for the earliest of them, and resets it with a system call
when a newly armed timer is earlier than every timer armed since the last
check. Canceling a timer never makes that call: if the operating system's
timer then fires for a timer that no longer exists, the reactor finds
nothing to do, and goes back to sleep. Timers on `lowres_clock` don't use
the operating system's timer at all: the reactor checks them when it
updates the low-resolution clock, which is also why they're as precise
as that clock, about half a millisecond.

Here is what this costs, on the same laptop:

| Operation | Time |
|---|---|
| arm and cancel a `steady_clock` timer, 1 ms or 1 hour away | 20 ns |
| arm and cancel a `lowres_clock` timer | 6 ns |
| arm each of a million `steady_clock` timers, at random times | 18 ns |
| arm a timer that expires before every timer armed before it | 220 ns |
| run a timer's (empty) function when it expires | 20 to 26 ns |

Most of the difference between the first two lines is reading the clock:
`arm()` with a duration reads the clock to compute the expiration time,
and reading `lowres_clock` is fifty times cheaper. The fourth line is the
system call, which only the rare pattern of ever-earlier timers pays every
time.

Expiring is as cheap as arming, but with a catch: the reactor runs the
functions of all the timers that are due at once one after another,
without yielding between them. We armed a million timers for the same
moment, with functions that did nothing: they ran in 20 to 26
milliseconds, in one stretch, a stall that the stall detector reported.
The stall grows with the number of timers that expire together, and with
what their functions do. A thousand at once cost microseconds, but a
function that does real work, multiplied by many timers that expire
together, can stop the shard. That's why timer functions should do
little: the function of `sleep()`'s timer only fulfills a promise, and
the coroutine that waited then runs as an ordinary task, which can yield
like any other. Shoal will meet this problem in the next section.

In practice, timers are cheap enough to create for every request,
for example as a timeout, and to keep a million armed; and a
`lowres_clock` timer is the better choice when half a millisecond of
precision is enough.

## Shoal, version 5: keys that expire {index="Shoal!version 5"}

Redis lets a key have an expiration time, after which it's deleted, as if
by `DEL`. Caches use this to forget old data. A web site can keep the
data of a logged-in user in a key that expires after half an hour, so
that a user who leaves without logging out is logged out anyway. And a
program can take a lock by creating a key, which other programs see and
wait for, with an expiration time of a few seconds, so that if the
program crashes while holding the lock, the lock is released anyway.
Shoal's version 5 adds the following Redis commands for expiration
times, the ones that programs use most; Redis has a few more, such as `PEXPIRE` and `PTTL`,
which exercise 1 adds:[^shoal-v5]

[^shoal-v5]: The source of this version is in `shoal/v5/`.

| Command | What it does | Reply |
|---|---|---|
| `EXPIRE key seconds` | sets the key's expiration time, that many seconds from now; zero or less deletes the key | 1, or 0 if the key doesn't exist |
| `TTL key` | how long until the key expires | the seconds left, rounded; -1 if it has no expiration time; -2 if it doesn't exist |
| `PERSIST key` | removes the key's expiration time | 1, or 0 if it had none |
| `SET key value EX seconds` (or `PX milliseconds`) | sets the value and an expiration time; a `SET` without them removes any old expiration time | `OK` |

An expired key must disappear for every command: `GET` doesn't find it,
`EXISTS` and `DEL` don't count it, and `KEYS` doesn't list it. Redis
achieves this in two ways, which Shoal copies. [*Passive* expiration]{index="expiration of keys!passive"}
checks a key's expiration time whenever a command looks the key up, and
deletes the key then, if its time has passed. That alone would be
correct, but a key that nobody looks up again would stay in memory
forever, so [*active* expiration]{index="expiration of keys!active"} also deletes expired keys in the
background.

### The store

Each key's entry now has, next to its value, a pointer to its expiration
time, which is null if the key has none. So a key without an expiration
time costs only 8 bytes more than before:

```{.cpp include="shoal/v5/commands.hh" snippet="store"}
```

The expiration times of the keys that have one are also kept in order of
time, in a tree, so that Shoal can find the earliest quickly, which
active expiration needs. Each `expiration` record holds the time and a
pointer back to its key, not a copy of the key. An `sstring` keeps up to
14 characters inside its 16 bytes, but a longer key has an allocation of
its own, so a copy would double the memory of every long key. The pointer
stays valid for as long as the key exists, because
`std::unordered_map` never moves its elements, even when it rehashes: it
moves only its pointers to them.

Rehashing has costs of its own, though, which Shoal has ignored so far.
The table's buckets are one contiguous array, of about 8 megabytes for a
million keys, and each time the table grows, it allocates a larger array
and puts every key in its new bucket, all at once. While our experiments
below loaded a million keys, this stalled the shard in `SET` for up to 60
milliseconds, the stall that [chapter](#ch:how-seastar-runs-your-code)
found. And in a shard whose memory is fragmented, allocating such an
array can fail even while plenty of memory is free.
[Chapter](#ch:memory) solves both problems with a table made of many
small tables, each of which grows on its own.

The tree is a `boost::intrusive::multiset`, a red-black tree like the one
in `std::multiset`, but [*intrusive*]{index="intrusive containers"}: its
links are inside the elements, which inherit them from
`set_base_hook`, as a timer's links are inside the timer. So adding a
record to the tree doesn't allocate memory, and the tree can remove a
record that a key's entry points to with `iterator_to()`, without
searching for it. It's a *multi*set because keys may share an expiration
time. A record takes 48 bytes: 32 for the tree's links, 8 for the time,
and 8 for the pointer to its key. The `normal_link` option turns off a
check, which the default mode makes in builds without `NDEBUG`, that a
record has left the tree before it's destroyed. Without that check, the
tree's `clear()` can forget all the records at once, without visiting
them, which `FLUSHALL` uses below.

Why a tree, and not a heap, such as `std::priority_queue`, the usual way
to find the earliest of many times? A heap finds the earliest at once,
but it can't remove an element from its middle, or change one, and Shoal
does both: `PERSIST`, `DEL` and a plain `SET` remove a key's expiration
time, and `EXPIRE` changes it. A heap would have to keep the old times as
stale entries, to be recognized and skipped when they reach the top. A
heap is also one contiguous array, which grows by moving everything into
a larger one, the problem that
[chapter](#ch:how-seastar-runs-your-code) found in hash tables. The
tree's operations take time that grows with the logarithm of the number
of keys, about 20 steps for a million keys, which is little next to the
rest of what a command does.

Expiration times are measured with `lowres_clock`. It's cheap, which
matters because every command that looks up a key reads the time, and
steady, so that a change to the computer's clock doesn't make keys expire
early, or late. (Redis itself stores its expiration times as dates, with
the computer's clock, so that they still mean something after a restart
from a file on disk. Shoal keeps nothing on disk until
[chapter](#ch:files-and-storage), which writes them to its log as dates
too.) The coarse precision
is harmless: since both the expiration time and the check read the
low-resolution clock, a key may expire up to about half a millisecond
early or late, while expiration times are given in seconds or
milliseconds.

Every command that looks up a key now does it through `lookup()`, which
deletes the key first if it has expired:

```{.cpp include="shoal/v5/commands.cc" snippet="lookup"}
```

`GET`, `EXISTS` and `DEL` call `lookup()` instead of looking in the
table directly, and `DEL` deletes with `erase_key()`:

```{.cpp include="shoal/v5/commands.cc" snippet="get"}
```

```{.cpp include="shoal/v5/commands.cc" snippet="del-exists"}
```

`KEYS` can't delete keys, because it walks the table's
buckets, and deleting a key would invalidate its loop, so it uses
`expired()` to skip expired keys, and leaves them to be deleted later.
`FLUSHALL` is [chapter](#ch:concurrency-within-a-shard)'s, with one
change: before it moves the table out and destroys it gradually, it
forgets the whole tree at once, and cancels the timer:

```{.cpp include="shoal/v5/commands.cc" snippet="flushall-expirations"}
```

(`destroy_gently()` now takes only the table, which is all that's left
to destroy.)

`SET` parses its new options, with the help of two small additions,
which `EXPIRE` uses too:

```{.cpp include="shoal/v5/commands.cc" snippet="parse-integer"}
```

and then sets the expiration time, or removes the old one:

```{.cpp include="shoal/v5/commands.cc" snippet="set"}
```

The three new commands are short:

```{.cpp include="shoal/v5/commands.cc" snippet="expire"}
```

`TTL` rounds the time left to the nearest second, as Redis does: right
after `EXPIRE key 10`, Redis's own `PTTL`, which reports milliseconds,
said 9,987, and its `TTL` said 10.

### Deleting expired keys in the background

The obvious way to delete a key when its time comes is to give each key
a timer, whose function deletes the key. Timers are cheap to arm and to
cancel, as we measured, so this would be quick to write, but it has two
problems. The first is memory: a timer takes 96 bytes, twice a tree
record, so a million keys with expiration times would need 96 megabytes
for their timers alone. The second is worse: the reactor runs the
functions of all the timers that are due one after another, without
yielding, and keys often expire together, because a program loads many
keys with the same expiration time, or because Redis's `EXPIREAT` gives
them the same moment, such as midnight. We tried it, with a copy of
Shoal that had a timer for each key, and rounded each expiration time up
to a whole second, as `EXPIREAT` would. A client loaded a million keys in
4 seconds, so about a quarter of a million expired together in each of
the seconds that followed, and each time, their timers' functions ran for
55 to 98 milliseconds without a break.

So Shoal has one timer, armed for the earliest expiration time. The
store's constructor gives it its function:

```{.cpp include="shoal/v5/commands.cc" snippet="store-constructor"}
```

Setting a key's expiration time takes the key's record out of the tree,
if it's there, or makes a new one, and puts it in its place. If that
place is first, the timer must fire earlier, so it's rearmed:

```{.cpp include="shoal/v5/commands.cc" snippet="expire-at"}
```

When the key that expires first is deleted, or loses its expiration
time, Shoal leaves the timer alone. The timer then fires for nothing, and
its function finds no key due, and arms the timer for the new earliest.
It's the reactor's own trick: when a timer is canceled, the reactor
doesn't reprogram the operating system's timer either, but lets it fire
for nothing, and finds nothing to do.

The timer's function deletes the keys that are due, in order of time,
and arms the timer for the next one. It can't yield, so it stops after a
batch of a thousand keys, and if more are due, arms the timer for the
current time. The reactor checks `lowres_clock` timers when it updates
the clock, between tasks, so other work runs before the next batch:

```{.cpp include="shoal/v5/commands.cc" snippet="expire-keys"}
```

How well does it work? We loaded 200,000 keys that expire after 300
milliseconds, and never looked them up again. A copy of Shoal that logged
each call of the timer's function found that it deleted all of them, at
about a quarter of a microsecond per key, in batches that took at most
0.6 milliseconds, while it served the client that was still loading keys.
Keys that aren't due cost nothing: the timer fires only when the earliest
key is due, however many keys wait behind it.

We then repeated the experiment of a quarter of a million keys expiring
together each second, with Shoal's one timer, and compared:

| | A timer per key | One timer (Shoal) |
|---|---|---|
| memory for each key's expiration time | 104 bytes (a 96-byte timer and a pointer to it) | 56 bytes (a 48-byte record and a pointer to it) |
| longest time without yielding, when a quarter of a million keys expire together | 55 to 98 ms | under 1 ms |

Seastar does have a structure for exactly this job. The timer set that
we saw earlier in this chapter is a class,
[`seastar::timer_set`]{.index}, in a public header,
`<seastar/core/timer-set.hh>`, and Seastar's own
memcached server, `apps/memcached/`, keeps its items' expiration times in
one, with one timer armed for the earliest, as Shoal does. It would have
saved Shoal the tree's steps, which grow with the logarithm of the
number of keys, since the timer set adds and removes an element with a
few list operations; and 16 bytes per record, a list's links instead of
a tree's. But its interface was made for the reactor's own
timers, not for programs. Its elements must have a `cancel()` method,
which the set's destructor calls until each element is gone (memcached
gives its items one, with the comment "needed by timer_set"); its
`remove()` reads a member named `_expired` directly; and `expire()`
returns all the due elements at once, so deleting them in batches would
mean keeping that list between batches. It's also not exported from
Seastar's C++ module.

Valkey doesn't keep its expiration times in order at all
(`src/expire.c` in its source). Ten times a second, its
active
expiration continues a scan of its table of expiration times, a few
dozen keys at a time, and deletes the keys that have expired. It
continues while more than a tenth of the keys it looked at had expired,
and stops when it has used a quarter of the core's time. This needs no
sorted structure, and nothing per key beyond the table of expiration
times that Valkey keeps anyway. Its price is that expired keys linger in
memory, up to about a tenth of those with expiration times, and that the
scan keeps costing time even when few keys expire. Shoal didn't copy it,
partly because a sorted tree deletes each key soon after its time, with
work only for the keys that expire, and partly because this chapter is
about timers.

Neither of the problems that ruled out a timer per key is a law of
nature. Seastar could run the functions of due timers in batches, and
let other work run between them, as Shoal's function does, and a timer
could be smaller; and `timer_set` could get an interface meant for
programs. Until then, a program with millions of expiration times does
well to keep them in a structure of its own, and use one timer.

## Timeouts {index="timeouts"}

An operation that may take too long, such as a request to another
server, should usually come with a time limit. The simplest tool is
[`with_timeout()`]{index="`seastar::with_timeout()`" .main}, from `<seastar/core/with_timeout.hh>`,
which takes a time and a future, and returns a
future that fails with a [`timed_out_error`]{index="`seastar::timed_out_error`"} if the first one isn't available by
then (that exception's header, `<seastar/core/timed_out_error.hh>`, is
included with it):

```{.cpp include="examples/time/timeouts.cc" snippet="with-timeout"}
```

Run, and followed by a long enough sleep, this prints:

```
timed out
slow_operation() finished
```

`with_timeout()` gave up waiting after 20 milliseconds, as asked. But the
operation didn't stop: it ran on, and finished 80 milliseconds later,
when nobody wanted its result any more. That's what Seastar's
documentation says: "timing out doesn't cancel any tasks associated with
the original future". Seastar can't stop a coroutine from the outside, as
[chapter](#ch:futures-and-promises) explained, so a timeout only stops
the *waiting*. The abandoned operation still uses whatever it uses, and
everything it uses must still live until it ends, which is easy to
forget, since the code that started it has moved on.

So `with_timeout()` suits operations that are harmless to abandon, and
that end by themselves. Many of Seastar's own functions that wait accept
a timeout themselves, which is better, because they then stop waiting
properly: a `shared_promise`'s waiter, in
[chapter](#ch:futures-and-promises), or a wait for a semaphore, in
[chapter](#ch:coordination), removes itself from the queue of waiters
when its time is up. For everything else, stopping the operation itself
is a job for an abort source.

## Abort sources {index="`seastar::abort_source`"}

An [`abort_source`]{index="`seastar::abort_source`" .main}, from `<seastar/core/abort_source.hh>`, is how
one piece of code asks another to stop. The
asking side calls `request_abort()`, or `request_abort_ex()` with an
exception that says why. The other side checks, at its own pace: it can
test `abort_requested()`, or call `check()`, which throws if an abort was
requested; and code that waits can `subscribe()` a function, which
`request_abort()` calls at once, so that the waiting stops right away.
`subscribe()` returns a *subscription*, and destroying the subscription
cancels it, so a waiter that is done waiting doesn't get called later.

Seastar's functions that wait often accept an abort source, and stop
waiting when it's aborted. [`sleep_abortable()`]{index="`seastar::sleep_abortable()`"} is one: it's a sleep that
fails with [`sleep_aborted`]{index="`seastar::sleep_aborted`"}, or with the exception given to
`request_abort_ex()`, when its abort source is aborted. (It's built from a
timer, and a subscription that cancels the timer.) Here is the operation
from above, rewritten to stop when it's asked to, and aborted by a timer
after 20 milliseconds:

```{.cpp include="examples/time/timeouts.cc" snippet="abort"}
```

This prints `abort requested`, from the subscription, then `aborted`,
and nothing more: this time the operation really stopped
([Figure](#fig:timeout-abort)).

![Two ways to give up on `timeouts.cc`'s operation after 20 ms. Top:
`with_timeout()` stops the caller's wait, but the operation runs on, and
finishes at 100 ms. Bottom: an abort source, aborted by a timer, stops
the operation itself. A thick line shows code that is still
waiting.](figures/timeout-abort.svg){#fig:timeout-abort}

A timer that aborts an abort source is common enough that Seastar packages it, as
[`abort_on_expiry`]{index="`seastar::abort_on_expiry`"}, from `<seastar/core/abort_on_expiry.hh>`, which aborts its abort source, with a `timed_out_error`,
at a given time:

```{.cpp include="examples/time/timeouts.cc" snippet="abort-on-expiry"}
```

This prints `stopped: timedout`: the exception is the `timed_out_error`
that `abort_on_expiry` gave, not `sleep_aborted`.

An operation of your own becomes abortable the same way: it takes an
`abort_source&`, passes it to the functions it waits for, and checks it
between steps. Large Seastar programs pass abort sources everywhere:
ScyllaDB's code mentions `abort_source` about a thousand times, and
Redpanda's over two thousand. [Chapter](#ch:clean-shutdown) uses one
to stop a whole server.

## Testing code that waits {index="`seastar::manual_clock`"}

Code that waits is slow to test, and its tests tend to be flaky: a test
that waits for a 10-second timeout takes 10 seconds, and a test that
expects something to happen within 10 milliseconds fails on a busy
machine. Seastar's answer is [`manual_clock`]{index="`seastar::manual_clock`" .main}, from
`<seastar/core/manual_clock.hh>`, a clock whose time moves
only when someone calls `manual_clock::advance()`. Code written for any
clock, as a template, can be tested with this one:

```{.cpp include="examples/time/manual.cc" snippet="manual"}
```

This prints `after 9 s: false`, then `after 10 s: true`, at once, without
waiting at all. `advance()` doesn't run the expired timers' functions
itself, but schedules them, so the test must yield before it checks.
`manual_clock` is one clock for all the shards, and advancing it expires
the timers of every shard. ScyllaDB's tests use it about a hundred times,
and Redpanda's about five hundred.

> **C++ corner: functions that can only move.**[]{index="C++ corner!functions that can only move"}[]{index="`std::function`"}
> `std::function<void ()>`, which C++11 added, can hold anything that can
> be called with no arguments: a function, or a lambda with any captures.
> But copying a `std::function` copies what it holds, so it accepts only
> what can be copied. A lambda that captures a `promise`, a `unique_ptr`
> or anything else that can only move can't be stored in one; GCC
> rejects it with "std::function target must be copy-constructible".
> Seastar's lambdas capture such things all the time, so Seastar has a
> function type of its own,
> [`noncopyable_function`]{index="`seastar::noncopyable_function`" .main},
> from `<seastar/util/noncopyable_function.hh>`, which holds anything that
> can be called and moved, and can itself only be moved. `timer`'s
> constructor takes one, which is why `delayed` above takes one too, and
> moves it into its timer. A `noncopyable_function` keeps a lambda of up
> to 32 bytes inside itself, without allocating memory, and its
> signature can say that the call doesn't throw:
> `noncopyable_function<void () noexcept>`. C++23 added
> [`std::move_only_function`]{index="`std::move_only_function`"}, from
> `<functional>`, for the same purpose. Seastar's own is older, from
> 2017, and Seastar's interfaces use it, so it's the one that a Seastar
> program meets whenever it hands Seastar a function to keep.

## Exercises

1. Add Redis's `PEXPIRE key milliseconds` and `PTTL key`, which are
   `EXPIRE` and `TTL` in milliseconds. Write their tests first, and
   check them against Valkey. What does `PTTL` return right after
   `PEXPIRE key 1000`, in Valkey and in Shoal, and why might the two
   differ?

2. Redis can close the connections of clients that send nothing for a
   while: `CONFIG SET timeout 2` closes a connection after 2 idle
   seconds. Add a `--timeout` option to Shoal that does the same. Don't
   use `with_timeout()` on the read: after it times out, the read is
   still in progress, on a stream that you're about to close. Instead,
   use a timer that is rearmed whenever a request arrives, and when it
   expires, calls the socket's `shutdown_input()`, so that the read ends.
   How do you test it?

3. In `timeouts.cc`, make `slow_operation()` take a `std::string&`,
   and print it when it finishes, and make `try_with_timeout()` pass it a
   local variable. Run it built normally, and in `debug` mode
   ([chapter](#ch:object-lifetime)). What happens, and why does the
   timeout make this bug possible?

4. Make a copy of Shoal version 5 that gives each key with an expiration
   time a `timer<lowres_clock>` of its own, whose function deletes the
   key, instead of the tree and the one timer. To make keys expire
   together, as `EXPIREAT` would, round each expiration time up to a
   whole second, with `std::chrono::ceil()`. Load a million keys with
   `EX 5`, and measure how long the reactor runs timer functions without
   a break when each second's keys expire. The stall detector won't tell
   you much: loading the keys stalls too, when the hash table grows, and
   by default it reports only a few stalls a minute. Instead, make the
   timer's function note the time, and log every run of functions that
   follow each other closely. Then make the same rounding in the real
   version 5, log how long each call of its timer's function takes, and
   compare.
