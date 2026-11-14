# Clean shutdown {#ch:clean-shutdown}

So far, every server in this book has stopped by being killed. Ctrl-C or
`kill` ended the process at once, wherever it was: in the middle of a
request, with replies still in a buffer, with background work half done.
For Shoal, which keeps everything in memory, little is lost that wasn't
going to be lost anyway. But a server that writes to disk, as Shoal will
in [Part](#part:io), may leave a file half written, and in every server,
the clients see their connections reset rather than closed, and a reply
they were waiting for never comes.

A *clean shutdown* is the opposite: the server stops taking new work,
finishes or stops the work in progress, closes what must be closed, and
exits with a status of 0. Redis does this when it gets the `SHUTDOWN`
command, or the signal that Ctrl-C or `kill` sends; its documentation
lists the steps: "Stop all the clients", save the data if it's
configured to, "Quit the server". This chapter makes Shoal do the same.

Every clean shutdown is made of the same three moves. It *asks* the
work in progress to stop, with an abort source ([chapter](#ch:time)); it
*waits* for that work to end, with a gate ([chapter](#ch:coordination));
and when some work doesn't end in time, it *gives up* on it. The
chapter starts with the signals that start a shutdown, then shows the
abort source and the gate working together, and builds Shoal's shutdown
from them, adding what a shutdown teaches about gates to what
[chapter](#ch:coordination) said. Then it lists the Seastar objects that must be closed before
they are destroyed, and what happens when you forget; explains why a
clean shutdown matters even to a program that is about to exit, because
LeakSanitizer, a tool that finds the memory a program never freed, is
useful only in a program that shuts down cleanly; and ends with
how ScyllaDB and Redpanda order the shutdown of dozens of services.

## Signals {index="signals"}

When you press Ctrl-C, the terminal sends the [`SIGINT`]{index="`SIGINT`"} signal to
the program running in it, and `kill` sends [`SIGTERM`]{index="`SIGTERM`"} by default. A
program that doesn't handle these signals is ended by the kernel,
immediately. A Seastar program handles them by default: when
`app_template::run()` starts, shard 0 installs handlers for both, which
start an orderly shutdown. That shutdown stops the reactors, but first
it waits for the function given to `run()` to finish.

This is fine for a program that does a job and ends. But a server's
main function ends only when the server stops, so with the default
handlers, Ctrl-C and `kill` do nothing visible: Seastar waits for a
function that never finishes. That's why the servers of
[chapter](#ch:tutorial-introduction), and Shoal up to version 6,
set [`auto_handle_sigint_sigterm`]{index="`app_template::config`!`auto_handle_sigint_sigterm`"} to `false`: it tells
Seastar not to handle these signals at all, so that the kernel ends the
program at once. It's also what you saw in exercise 1 of
[chapter](#ch:errors), whose program waited forever for a promise that
was destroyed without being kept: Seastar's handler started the
shutdown, and the shutdown waited, forever, for the program's main
function.

### Handling a signal {index="`seastar::handle_signal()`"}

A server should handle these signals itself, and treat them as a request
to stop. Seastar's [`handle_signal()`]{index="`seastar::handle_signal()`" .main}, from `<seastar/core/signal.hh>`,
installs a handler: a function that Seastar calls when the signal
arrives. Shoal version 7 installs the same handler for both signals:

```{.cpp include="shoal/v7/main.cc" snippet="signals"}
```

The handler only requests an abort from an abort source, `db.stopping`,
which the rest of the chapter explains. Unlike a handler installed with the
C library's `signal()`, a Seastar handler isn't called in the middle of
whatever the program was doing, where almost nothing is safe to call.
Seastar's real signal handler, the one the kernel calls, only notes that
the signal arrived, and the reactor calls your function later, as an ordinary task, when it next polls for
events, on the shard that installed it. So a Seastar signal handler may
do anything that other code may. It also means that a shard that never
polls, because something blocks it, as in exercise 3 of
[chapter](#ch:coordination), can't run its handlers either: no signal
except `SIGKILL` (`kill -9`) stops it.

`handle_signal()` replaces Seastar's own handlers for these two signals,
and any handler that was installed for the signal before. But Seastar
has no function that removes a handler, and Shoal's handlers refer to
`db`, a local variable of the function that installed them. If a signal
arrived after `db` was destroyed, the handler would use destroyed memory.
So before `db` goes away, Shoal replaces its handlers with handlers that
do nothing. ScyllaDB does exactly this, with a comment: "There's no way
to unregister a handler yet, so register a no-op handler instead." Shoal
does it with [`defer()`]{index="`seastar::defer()`"}, from `<seastar/util/defer.hh>`, which returns
an object, a [`deferred_action`]{index="`seastar::deferred_action`"}, that calls a function when it's
destroyed. The function must be `noexcept`, since it's called from a
destructor, which can't throw.
`forget_signals` is declared after `db`, so it's destroyed, and its
function called, before `db` is.

Seastar's README shows a similar pattern, but it handles only
`SIGINT`. A server should handle both: `SIGTERM` is what `kill`
sends by default, and what system service managers such as systemd send
to stop a service. And the third argument of `handle_signal()`,
`once`, which calls the handler only for the first signal, isn't needed
here: requesting an abort twice does no harm.

### Signals that are ignored {index="signals!ignored"}

One more surprise awaits the programs that leave the signals to the
kernel. A shell script that starts a program in the background, with
`&`, starts it with `SIGINT` *ignored*: the shell does it so that a
Ctrl-C meant for the script doesn't stop its background jobs too, and
the program inherits the setting. A program that doesn't handle `SIGINT`
then ignores `kill -INT`, whose default action is now to do nothing. We
met this while testing: Shoal version 6, started with `&` from a
script, ignored `kill -INT`, and its `/proc/PID/status` showed `SIGINT`
in the `SigIgn` mask. Version 7, which installs its own handler, stopped
normally: installing a handler replaces the inherited "ignore". `SIGTERM`
is never ignored this way, which is one more reason to use it to stop a
server.

## Asking and waiting {index="shutdown!asking and waiting"}

Stopping a server means stopping many pieces of work, which run
concurrently: the loop that accepts connections, a coroutine for each
connection, background work. Two of Seastar's tools, which earlier
chapters introduced, do the two halves of the job.

An [abort source]{index="`seastar::abort_source`!stopping work"} ([chapter](#ch:time)) *asks* work to stop. The
asking side calls `request_abort()`; the work either checks
`abort_requested()` between its steps, or `subscribe()`s a function,
which `request_abort()` calls at once; and many of Seastar's waits
take an abort source, and end, with an exception, when it's aborted.

A [gate]{index="`seastar::gate`!shutdown"} *waits* for work to end. As [chapter](#ch:coordination)
showed, each piece of work holds a `gate::holder`, which it gets from
the gate's `hold()`, and which the gate counts until it's destroyed; the
owner of what the work uses calls the gate's `close()`, which refuses
new holders from then on (`hold()` throws a `gate_closed_exception`),
and returns a future that is resolved when the last holder is gone.

Neither can do the other's job:

| | Abort source | Gate |
|---|---|---|
| Asks the work to stop | yes: `request_abort()` | no |
| Ends a wait the work is in | yes, if the wait takes it, or subscribes | no |
| Tells when the work has ended | no | yes: `close()`'s future |
| Refuses new work | no | yes, after `close()` |
| Lets the work check, between steps | `abort_requested()`, `check()` | `is_closed()`, `check()` |

An abort source alone asks the work to stop, but the code that asked
can't know when it did, so it can't know when it's safe to destroy what
the work uses. A gate alone waits until the work ends, but if the work
is waiting for something that won't come, it waits forever. So shutdown
code uses both, one after the other. Here is the pair, from a small
example program, `periodic.cc`. It starts some work in the background,
holding a gate, and a tenth of a second later stops it, and measures
how long that took:

```{.cpp include="examples/clean-shutdown/periodic.cc" snippet="stop"}
```

Both large Seastar projects pair them the same way. Of the 196 header
files of Redpanda that declare a gate as a member of a class, 95 declare
an abort source too, and in the year before this book was written,
ScyllaDB's headers gained 25 new gates, and 10 new abort sources. The
abort source is not a newer replacement for the gate.

### Work that waits {index="background work!that waits"}

The first piece of work that `periodic.cc` stops saves a snapshot every
second, until stopping is requested, and sleeps between saves with the
`sleep()` of [chapter](#ch:time):

```{.cpp include="examples/clean-shutdown/periodic.cc" snippet="slow"}
```

It checks `abort_requested()` between steps. But a step is mostly a
sleep, and the check comes only after the sleep. The second version
gives the abort source to the sleep itself, with `sleep_abortable()`,
which fails with [`sleep_aborted`]{index="`seastar::sleep_aborted`"} as soon as an abort is requested:

```{.cpp include="examples/clean-shutdown/periodic.cc" snippet="abortable"}
```

The program prints:

```
sleep(): stopping took 0.9 seconds
sleep_abortable(): stopping took 0.0 seconds
condition_variable: stopping took 0.0 seconds
```

The first one waited out the rest of its second. Stopping a server is
often like this: much of its work is waiting for something, and each
wait that doesn't take the abort source adds its remaining time to the
shutdown. (Catching `sleep_aborted` is what ends the loop. Without the
`catch`, the exception would end the coroutine just the same, but in a
future that nobody looks at: exercise 2 shows what Seastar says about
that.)

Redis has background work of this kind. Besides freeing memory in the
background, it saves snapshots of its data periodically, and rewrites
its append-only file, in child processes, and its `SHUTDOWN` stops them
rather than waiting: its documentation says that "if there is an AOF
child process performing an AOF rewrite, Redis will simply kill it and
exit". Shoal will have such work too: version 11, in
[chapter](#ch:scheduling-groups), rewrites its log on disk in the
background, as Redis does, and its shutdown must stop the rewrite, not
wait for it to finish.

Not all of Seastar's waits take an abort source. A semaphore's `wait()`
and `get_units()` do, and so do an `rwlock`'s locks and a
`shared_future`'s `get_future()`. A condition variable's `wait()`
doesn't, and neither does a queue's `pop_eventually()`. This is
deliberate. Since 2022, four pull requests have tried to add an abort
source to the condition variable's waits, and none was accepted: Avi
Kivity explained that he was trying to avoid "a matrix of every
primitive interacting with every other primitive", and showed how to
glue the two together instead. The third piece of work in `periodic.cc`
does it:

```{.cpp include="examples/clean-shutdown/periodic.cc" snippet="condition"}
```

The subscription wakes every waiter when the abort is requested, and
the loop checks the abort source after each wait, which it must do
anyway, since a condition variable's waiters can wake up for other
reasons. A queue doesn't need the glue: it has an `abort()` of its own,
which fails the waits on it.

### What a shutdown adds to gates {index="`seastar::gate`!shutdown"}

[Chapter](#ch:coordination) showed what a gate does. A shutdown, where
a server closes several gates while its work is still running, adds a
few lessons:

- **Close the gates in the order of their dependencies.** When work of
  one kind can start work of another, as a request can start background
  work, close the gate of the starting work first, and wait for it, then
  close the gate of the work it starts. Shoal does this below.
- **Work in progress may try to start new work after the close.** Then
  `hold()` throws. Either check `is_closed()` before starting it, as
  Shoal's `FLUSHALL ASYNC` will, or use `try_hold()`, and do the work
  some other way, or not at all. A holder that already exists can still
  be copied after the close, though, and the copy is counted, so work in
  progress can hand holders to the helpers it starts, and `close()`
  waits for them too.
- **A gate is closed only once.** Calling `close()` a second time fails
  an assertion ("seastar::gate::close() cannot be called more than
  once"), so the code that stops a component must not run twice: a
  second signal, or a second `SHUTDOWN`, must not stop it again.
  Shoal's second request only requests an abort that was already
  requested, which does nothing.
- **Use the gate to stop work only if the work never waits.** Work that
  runs in steps, and waits only for its turn on the CPU, can check
  `is_closed()` between steps, and needs no abort source. Work that waits
  for anything else needs the abort source, as `periodic.cc` showed.
- **A shutdown that hangs is almost always a `close()` that never
  returns**, because some work never ends. Log `get_count()`, the number
  of holders left, while waiting; give each gate a name, with
  `named_gate`, in a program with many of them; and, as Shoal does
  below, don't wait forever.

## Shoal, version 7 {index="Shoal!version 7"}

Version 7 stops cleanly.[^shoal-v7] It stops when it gets `SIGINT`,
`SIGTERM`, or Redis's `SHUTDOWN` command. Like Redis, it then:

1. stops accepting new connections;
2. stops reading requests from the clients, finishes the requests it has
   already read, sends their replies, and closes each connection;
3. waits for the connections to end, but if some haven't ended after a
   grace period, gives up on them, and closes them;
4. stops its background work, and waits for it;
5. destroys its data, and exits with a status of 0.

[^shoal-v7]: The source of this version is in `shoal/v7/`.

### Stopping is a request {index="`seastar::abort_source`!stopping a server"}

Shoal uses one abort source to ask everything to stop, `stopping`, and
two gates to wait: `connections`, a local variable of `shoal_main()`,
which every connection holds, and `background`, which background work
holds. `stopping` and `background` are members of the store, next to a
second abort source, `giving_up`, which we'll meet later:

```{.cpp include="shoal/v7/commands.hh" snippet="stopping"}
```

The signal handlers request the abort, as we saw. So does the new
`SHUTDOWN` command:

```{.cpp include="shoal/v7/commands.cc" snippet="shutdown"}
```

Redis's `SHUTDOWN` accepts `NOSAVE` and `SAVE`, which say whether to save
the data to disk before exiting; Shoal accepts them too, and ignores
them, since it saves nothing (yet). And it doesn't reply. Redis's
documentation says that "on successful shutdown, nothing is returned
because the server quits and the connection is closed", and that's what
Shoal does too: the command only requests the abort, and the rest of the
shutdown closes this client's connection, as it closes all the others.
Shoal's tests found one difference from Valkey here: a `SET` that a
client sends just before `SHUTDOWN`, on the same connection, gets its
`+OK` from Shoal, which finishes the requests that it already read, but
not from Valkey.

### Stop accepting {index="`seastar::server_socket`!`abort_accept()`"}

The accept loop of the earlier versions waited for the next client
forever. Now a subscription ends that wait:

```{.cpp include="shoal/v7/main.cc" snippet="accept"}
```

[`abort_accept()`]{index="`seastar::server_socket`!`abort_accept()`"} makes the `accept()` that the loop waits for fail. The loop
gets its result with `coroutine::as_future()` ([chapter](#ch:coroutines)),
which returns the future without throwing, and checks whether stopping
was requested before looking at it. If it was, it ignores the future,
whatever it holds (an exception from `abort_accept()`, or, if a client
connected at the last moment, a connection that the server no longer
wants), and leaves the loop. The listening socket is closed when
`listener` is destroyed, and from then on, new clients are refused by
the kernel.

The loop gives each connection a holder of the `connections` gate.
`handle_connection()` takes it by value, as its last parameter, so the
gate counts the connection until its coroutine ends. `refuse()`, which
tells a client beyond the limit of [chapter](#ch:coordination) that
there are too many clients, takes a holder too, so that the shutdown
waits for that message to be sent as well.

### Finish the connections

A connection's coroutine reads requests, and executes them, until the
client closes the connection. To end it sooner, Shoal pretends that the
client did. Each connection subscribes to `stopping` when it starts:

```{.cpp include="shoal/v7/main.cc" snippet="stop-reading"}
```

`shutdown_input()` shuts down the socket's receiving side, as if the
client had closed the connection: the read that `serve()` is waiting
for, if any, ends, and `serve()` returns. A request that is being
executed when this happens isn't interrupted, and its reply is still
sent: `serve()` notices only when it next tries to read. Then the
connection's coroutine does what it always did when a client hung up: it
closes its streams, which sends whatever is still buffered, and ends.
(The requests that the input stream had already read into its buffer
are executed first, too.) Two kinds of client don't send requests while
they wait, and the earlier versions already knew how to end their wait:

- A client waiting in `BLPOP` ([chapter](#ch:coordination)) waits on an
  abort source, `hung_up`, which is aborted when the client hangs up.
  `shutdown_input()` triggers it just the same. In version 6, `BLPOP`
  then replied with a null array, as for a timeout; Valkey sends no
  reply at all when it shuts down, and now Shoal doesn't either: when
  `hung_up` was triggered, there's no one to reply to.
- A Pub/Sub subscriber ([chapter](#ch:concurrency-within-a-shard)) is
  served by the same loop, which ends the same way.

One trap hides in `subscribe()`. If the abort was already requested
when you subscribe, `subscribe()` doesn't call the function: it returns
an empty subscription, which evaluates to `false`, and the function is
never called (exercise 3 checks this). A connection that subscribed
after the server started stopping would go on reading forever. Shoal
can't miss the request: the accept loop checks for it after each
`accept()`, and `handle_connection()` subscribes before its first
`co_await`, so no other code, and no request, can run in between. Code
that can't be sure should check `abort_requested()` right after it
subscribes.

### Wait, but not forever {index="shutdown!grace period"}

After the loop, `shoal_main()` waits for the connections, and then for
the background work:

```{.cpp include="shoal/v7/main.cc" snippet="stop"}
```

The order matters. A connection may still be executing a request that
starts background work, such as `FLUSHALL ASYNC`, so the background gate
can't be closed before the connections are done. And the objects that
the connections and the background work use are local variables of
`shoal_main()` (`db`, `ps`, `lim`), which are destroyed when it returns,
so it may return only when both gates are closed.

But waiting for the connections can take forever. The first draft of
version 7 waited without a limit, and we found a client that hung it:
one that sends requests, and never reads the replies. Our test client
sent 100 `GET`s of a value of a megabyte. Shoal executed a few, until
the socket's buffers were full, and its next write waited for the client
to read. After a `SHUTDOWN` from another client, Shoal stopped reading,
as designed, but the write went on waiting, the connection's coroutine
never ended, and neither did `connections.close()`: Shoal was still
running ten seconds later. Valkey exited at once: it closes its clients'
connections without sending what they haven't read. A client can't be
allowed to keep a server from stopping, so version 7 waits for a grace
period, 10 seconds unless the new `--shutdown-grace` option says
otherwise, and then gives up. A timer requests an abort from
`giving_up`, and each connection subscribes to it when it starts:

```{.cpp include="shoal/v7/main.cc" snippet="stop-writing"}
```

`shutdown_output()` shuts down the socket's sending side. The write that
was waiting fails, with "Broken pipe", and so does every later write, so
the connection's coroutine ends, with that exception, which Shoal logs.
With a grace period of a second, the shutdown with this client looks
like this (shortened):

```
INFO  00:37:46,557 shoal - Shoal is stopping
INFO  00:37:47,557 shoal - giving up on the connections still open: 1
WARN  00:37:47,557 shoal - connection with 127.0.0.1:59640 failed:
    std::system_error (error system:32, Broken pipe)
INFO  00:37:47,557 shoal - Shoal has stopped
```

and Shoal exits with a status of 0. [Figure](#fig:shutdown-timeline)
shows the whole shutdown, with four kinds of client connected, and
`FLUSHALL ASYNC`'s work in the background.

![Shoal version 7 stopping. A thick line shows work that is still running
or waiting, which its gate counts; a dashed line, `shoal_main()` waiting
for a gate's `close()`. Stopping is requested when `SHUTDOWN` or a
signal arrives; most connections end right away, one finishes its
request first, and the one whose client doesn't read ends only when the
grace period is over. Only then is the background gate closed, which
stops the background work at its next step.](figures/shutdown-timeline.svg){#fig:shutdown-timeline}

This is the third move of a shutdown: giving up. Like asking, it's done with an abort source; unlike
asking, it doesn't wait for the work's cooperation. When the shutdown
ends sooner, the timer is cancelled, so its function, which uses the
gate, can't run after `shoal_main()` has returned.

### Stopping the background work {index="background work!stopping"}

`FLUSHALL ASYNC`, as [chapter](#ch:concurrency-within-a-shard) built
it, replies at once, and destroys the old data in the background, in a
coroutine whose future nobody waits for: `destroy_gently()`, which takes
the data by value, so that it lives in the coroutine's frame. That
chapter warned that nothing waits for such work when the program stops.
Version 7 makes it hold the `background` gate, and stop early when the
gate is closed. It only computes, and never waits, so, as the lessons
about gates above said, the gate is enough to stop it:

```{.cpp include="shoal/v7/commands.cc" snippet="destroy-gently"}
```

Instead of calling `clear_gently()`, as before, it erases the entries
itself, so that it can check [`is_closed()`]{index="`seastar::gate`!`is_closed()`"} after each one, at the
same place where `maybe_yield()` may let other work run. When Shoal
stops, it closes the gate, and the coroutine leaves its loop at its
next step. Returning destroys its frame, and with it the rest of the
data, at once, in a single stall: harmless at shutdown, when there are
no clients left to delay.

`flushall()` changes too. It mustn't start new background work after
the gate was closed, because `hold()` on a closed gate throws, and a
`FLUSHALL ASYNC` that arrives while Shoal is stopping (from a request
that was already read) destroys its data before replying, as `SYNC`
does:

```{.cpp include="shoal/v7/commands.cc" snippet="flushall"}
```

Does stopping early save anything? We measured it: 1.9 million keys,
loaded with `valkey-benchmark`, then `FLUSHALL ASYNC`, and right after
it, `SHUTDOWN`. `FLUSHALL SYNC` destroys these keys in about 0.2
seconds. After `FLUSHALL ASYNC` and `SHUTDOWN`, `destroy_gently()`
stopped early, as designed, with 1,649,317 entries still left, but
Shoal exited 0.22 seconds after the reply to `FLUSHALL`, no sooner than
if it had destroyed all of it gently: the remaining entries had to be
freed anyway, when the frame was destroyed, and Seastar reported that
as two stalls, of 65 and 123 milliseconds. When the work is only
freeing memory, and nothing else wants the CPU, stopping it early
doesn't save time; it only changes how the time is spent. Stopping
early pays off for work that waits, as `periodic.cc` showed. What does
matter here is the gate: without it, the program would exit while the
coroutine was suspended, and its frame and data would never be freed, a
leak, which [chapter](#ch:memory-leaks) shows how to catch.

## Objects you must close {index="closing objects"}

Shoal's connections close their streams before they end, and
[chapter](#ch:coroutines) explained why an output stream must be closed:
its destructor can't wait, so it can't send what's still in its buffer.
Many of Seastar's objects are like this: they own work in progress, or
resources that can only be released by waiting, and they have a
`close()` or `stop()` that returns a future, which you must call, and
wait for, before you destroy them. To learn what happens if you don't,
we destroyed each kind of object without it, in a small program, in a
`release` build:

| Object | Ended by | Destroyed without it |
|---|---|---|
| `output_stream` | `close()` | aborts, if it holds unsent data |
| `input_stream` | `close()` | nothing |
| `file` | `close()` | closes synchronously, which may block |
| `gate` | `close()` | aborts, if something holds it |
| `sharded<T>` | `stop()` | aborts, if it was started |
| `seastar::thread` | `join()` | aborts |
| `server_socket` | `abort_accept()` | a pending `accept()` fails |
| `http::client` | `close()` | nothing, when idle |

(`sharded<T>` comes in [chapter](#ch:talking-between-shards), `file` in
[chapter](#ch:files-and-storage), `seastar::thread` in
[chapter](#ch:threads), and the HTTP client in [chapter](#ch:http).)

Four of the eight abort the program, even in a `release` build, which
is as loud as a mistake can be: an output stream with unsent data with
"Was this stream properly closed?", a gate with "gate destroyed with
outstanding requests", and a `sharded<T>` and a `seastar::thread` with
a failed assertion. An output stream that was flushed, or never written
to, and a gate that nothing holds, are destroyed quietly. The others are
quieter, and worse for it. A `file` whose destructor closes it does so with the blocking system
call, which on a local disk is quick, but, as a comment in Seastar's
code warns, "close() can be a blocking operation on NFS", where it
stalls the shard. And "nothing" in the table means nothing that our
small test could see. A stream or an HTTP client destroyed without
`close()` didn't finish whatever it would have done in its `close()`,
which may matter elsewhere: a connection that isn't shut down properly,
or data that isn't written. Close them all, always.

### Closing, even when an exception interrupts {index="closing objects!helpers"}

Closing is easy to forget when code ends early because of an exception.
In C++, the usual answer is a destructor (RAII): an object whose
destructor cleans up, whichever way the scope is left. But a destructor
can't wait for a future, so in a coroutine, closing must be explicit.
Shoal's `handle_connection()` shows the pattern, since version 4: catch
the exception into a `std::exception_ptr`, close, and only then return
it, with `coroutine::return_exception_ptr()`
([chapter](#ch:coroutines) explained why the `co_await` can't be in
the `catch` block).

Seastar has helpers for the other styles of code, in
`<seastar/util/closeable.hh>`:

- [`with_closeable(obj, func)`]{index="`seastar::with_closeable()`"}, for code written with continuations,
  takes the object by value, calls `func` with a reference to it, and
  closes it when the future that `func` returns is resolved, whether it
  succeeded or failed: like `do_with()` ([chapter](#ch:object-lifetime))
  and `finally()` ([chapter](#ch:futures-and-promises)) combined. [`with_stoppable()`]{index="`seastar::with_stoppable()`"} does
  the same with `stop()`. Don't give them a coroutine lambda that
  captures anything: as [chapter](#ch:object-lifetime) showed, its
  captures die before it does.
- [`deferred_close`]{index="`seastar::deferred_close`"} and [`deferred_stop`]{index="`seastar::deferred_stop`"} are the RAII way: an object whose
  destructor calls `close()` or `stop()`, and waits for it. Waiting in a
  destructor is possible only in a `seastar::thread`
  ([chapter](#ch:threads)), where waiting is a call to `get()`, so these
  work only there. ScyllaDB uses them about 500 times, almost all in its
  tests, which it writes as `seastar::thread`s.

`defer()`, which Shoal used for its signal handlers, works anywhere,
because its function doesn't wait.

## Why not just exit? {index="LeakSanitizer!and clean shutdown"}

A server that is about to exit could skip all this: why wait for
background work, close streams, and free memory, when the process is
about to end and the kernel takes everything back? Partly to finish
what should be finished: replies that are buffered, and, once Shoal
keeps its data on disk, files that are half written. But even work that
only frees memory, like `destroy_gently()`, is worth waiting for,
because of the tool that finds memory leaks.

That tool is LeakSanitizer. It comes with AddressSanitizer, which
[chapter](#ch:object-lifetime) used to find the use of freed memory, so
it's on in every Seastar program built against a `debug` Seastar. When
the program exits, LeakSanitizer looks for memory that was allocated,
never freed, and that nothing points to anymore; it reports each such
block, with the place where it was allocated, and makes the program
exit with a status of 1, so that a test that leaks fails. A program
that exits while work is still running never frees that work's memory:
nothing destroys a suspended coroutine from the outside, so its frame,
and everything the frame owns, stays allocated. So a program that never
stops its background work is reported to leak every time, and its
developers soon learn to ignore the report, or turn LeakSanitizer off,
and then a real leak goes unnoticed. A program that stops cleanly, and
frees everything, exits silently, and any report is worth reading.
Shoal version 7, built against a `debug` Seastar, passes all its tests,
including the tests that stop it, which check that it exits with a
status of 0: had it leaked anything, LeakSanitizer would have made it 1.
[Chapter](#ch:memory-leaks) shows what LeakSanitizer reports, and the
leaks it can't find.

## Shutdown in large programs {index="shutdown!order"}

Shoal has three things to stop, in one function. A large server has
dozens of services, which depend on each other, and they must stop in
the reverse order of starting: a service that uses another must stop
before the one it uses. Both large Seastar projects arrived at the same
pieces.

**A stop signal.** ScyllaDB (since 2019) and Redpanda each have a class
named `stop_signal`, in `main.cc` and in `utils/stop_signal.h`. Each
installs handlers for `SIGINT` and `SIGTERM`, as Shoal does; when a
signal arrives, it requests an abort from an abort source, and wakes the
main function, which waits on a condition variable
([chapter](#ch:coordination)), so that the main function can start the
shutdown. The destructor replaces the handlers with handlers that do
nothing, as Shoal does. ScyllaDB's version keeps one abort source on
every shard, and requests an abort on all of them, since its services
run on all of them ([Part](#part:many-cores)). While the server is
still starting, it doesn't abort the services, which aren't ready to
stop; instead, the startup code calls the class's `check()` between
steps, which throws if a signal has arrived.

**Destruction in reverse order.** ScyllaDB's main function runs in a
`seastar::thread`, so it can use RAII that waits. After starting each
service, it creates an object whose destructor stops it, so the
services stop, when the function ends, in the reverse order of
starting, whichever way the function ends. Its `main.cc` does this 113
times, with a function of its own, `defer_verbose_shutdown()`, which logs
"Shutting down" and the service's name before stopping it, and whether
it succeeded after. If a stop fails, it aborts the program, to leave a
core dump of the bug, except after errors of the environment, such as a
full disk, after which it exits at once. The logs also say where a
shutdown that hangs is stuck.

**Stop the input first.** Redpanda's `application::shutdown()` starts
by stopping new requests, from its Kafka and internal RPC servers, as
Shoal stops accepting and reading, before stopping the services that
those requests use. It stops its services under a watchdog: if the stop
takes more than 15 seconds, it logs that the service "is taking more
than 15 seconds to shutdown"; after two minutes, it logs the same as an
error, since the stop is most likely stuck on a bug.

A shutdown that hangs is the typical bug of this area, as Shoal's
client that never reads showed: without its grace period, Shoal would
have waited for that client forever. A watchdog only reports such a
hang. Fixing it means giving up on the work that doesn't end in time,
as Shoal does.

## Exercises

1. Write a shell script that starts Shoal version 6 in the background,
   with `&`, waits a second, and sends it `kill -INT`. Does it stop?
   What does the `SigIgn` line of its `/proc/PID/status` show? Now do the
   same with version 7. Why the difference?

2. In `periodic.cc`, remove the `try` and `catch` around
   `sleep_abortable()`, so that `sleep_aborted` ends the coroutine. What
   does the program print now? Does the gate still close, and why?

3. Write a program that requests an abort from an abort source, and then
   subscribes a function to it. Is the function called? What does
   `subscribe()` return? In Shoal's `handle_connection()`, what would
   go wrong if the subscription came after the first `co_await`?

4. Many servers stop at once when Ctrl-C is pressed a second time,
   without waiting for the rest of the shutdown. Make Shoal give up on
   its connections at the second `SIGINT` or `SIGTERM`, without waiting
   for the grace period. Test it with `--shutdown-grace 100`, and a
   client that never reads its replies, like `test_v7.py`'s. How long
   does Shoal take to stop after one signal, and after two?

5. In `periodic.cc`, remove the subscription from `save_changes()`. What
   happens when you run the program? Can you stop it with Ctrl-C? Why
   doesn't the program's own abort source stop it?
