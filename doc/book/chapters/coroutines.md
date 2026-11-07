# Coroutines {#ch:coroutines}

[Chapter](#ch:tutorial-introduction) used coroutines without explaining
them, and you could follow it anyway: a coroutine reads almost like
ordinary code. This chapter explains what they are, how they wait without
blocking, how they call each other, how they report errors, and how they
read from and write to the network. Then we use all of that to turn Shoal
into a real server, which speaks the Redis protocol well enough that the
standard Redis tools work with it.

Seastar wasn't designed around coroutines. When it was born, in 2014,
C++ had none, and Seastar's asynchronous code was written as chains of
*continuations*: small functions, each called when the previous step
finished. Seastar added support for coroutines in 2019, even before C++20
made them part of the language, and today they are the normal way to
write Seastar code. Continuations are still useful, and you will see them
in older code, so [chapter](#ch:continuations) covers them; this book
starts with coroutines.

## Futures

A function that may have to wait, for data from the network, for the
disk, or for a timer, can't return its result directly, because the
result doesn't exist yet. Instead, it returns a *future*: an object that
stands for a result that will be available later. Such a function is what
[chapter](#ch:tutorial-introduction) called an *asynchronous function*.
In Seastar, the type is `seastar::future<T>`, where `T` is the type of
the result. A function
that produces no result, but still takes time to finish, returns
`future<>`. For example, `seastar::sleep()` returns a `future<>` that
becomes available when the time is up, and reading from a network
connection returns a `future` holding the data read.

A future is always in one of three states. It may be *unavailable*: the
result isn't there yet. It may be *available*, holding the result. Or it
may be *failed*, holding an exception instead of a result, because the
operation failed. An unavailable future becomes available or failed
exactly once, and then stays that way.

Despite the similar name, a `seastar::future` is not a `std::future`.
The standard one is meant for passing a result between threads, and its
`get()` blocks the calling thread until the result arrives. Blocking is
exactly what a Seastar program must never do, so a Seastar future doesn't
work that way. It is also much cheaper: it belongs to one shard, so it
needs no locks or atomic operations, and getting a result that is already
available costs almost nothing.

So how does a function use a future's result, if it can't wait for it?
It becomes a coroutine.

## Coroutines

A *coroutine* is a function that can suspend itself in the middle, and be
resumed later from the same point. In C++, any function that uses one of
the keywords `co_await`, `co_return` or `co_yield` is a coroutine, and in
Seastar, a coroutine returns a `future`. Here is one from Shoal. It reads
more data from a client's connection, and adds it to a buffer of data
read earlier:

```{.cpp include="shoal/v1/resp.cc" snippet="read-more"}
```

`_in.read()` returns a `future<temporary_buffer<char>>`: the data that
the client sends next, when it arrives. `co_await` waits for that future.
If the data has already arrived, the coroutine simply continues. If not,
the coroutine *suspends*: it returns to its caller, and the shard goes on
to run other work, such as serving other clients. When the data arrives,
Seastar resumes the coroutine where it stopped, and `co_await` gives it
the data. If the future failed, `co_await` throws its exception instead,
as we will see below.

`co_return` ends the coroutine and gives its result. The coroutine is
declared to return `future<bool>`, but `co_return` gives a plain `bool`:
the future that the coroutine returned to its caller becomes available
with that value.

What makes this work is the coroutine's *frame*. When a coroutine is
called, C++ allocates an object on the heap that holds its parameters and
its local variables, and the point where it is suspended. While the
coroutine is suspended, nothing of it remains on the thread's stack; the
frame keeps its state, and resuming it continues with the same local
variables. A coroutine is called like any other function. In Seastar, its
body runs right away, until it first suspends (or until it ends, if it
never suspends); then the call returns the future, while the frame waits
for the coroutine to be resumed.

Waiting at a `co_await` is what makes a single thread serve many clients
at once: every client is served by its own coroutine, and while one of
them waits, the others run. It also means that while a coroutine is
suspended, other code runs on the same shard, and may change data that
the coroutine uses. [Chapter](#ch:how-seastar-runs-your-code) explains
exactly when that can happen, and
[chapter](#ch:concurrency-within-a-shard) how to live with it.

### Coroutines calling coroutines

A coroutine can call another coroutine, and `co_await` its result. This
is how larger asynchronous operations are built from smaller ones. In
Shoal, reading a line of a request calls `read_more()` whenever the
buffer doesn't hold a whole line yet:

```{.cpp include="shoal/v1/resp.cc" snippet="read-line"}
```

and reading a whole request calls `read_line()` for each of its lines.
Each of these calls is a coroutine with its own frame. When
`read_more()` suspends, waiting for the network, the coroutines that are
waiting for it, `read_line()` and the one that called it, are suspended
too, each at its own `co_await`, and when the data arrives they resume in
turn. The code reads as if each call simply returned its result.

Note what this means: a coroutine is itself an asynchronous function,
since it returns a future. So a function that wants to use the result of
an asynchronous function must also be asynchronous: it must be a
coroutine that `co_await`s it, or return a future of its own in some
other way ([chapter](#ch:continuations) shows another). The one exception
is code that runs in a *Seastar thread*, which can wait for a future
without being asynchronous itself ([chapter](#ch:threads)). An asynchronous
operation is built from smaller asynchronous functions, like the
coroutines of Shoal's request reader, all the way down to the
asynchronous functions that Seastar provides, such as `sleep()` and
`read()`.

Creating a frame means allocating memory, which is cheap with Seastar's
memory allocator, but not free, so not every function should be a
coroutine. A function that never waits gains nothing from being one. In
Shoal, executing a command only looks up or changes a table in memory,
so `execute()`, which you will see below, is an ordinary function that
returns its reply directly. [Chapter](#ch:continuations) shows how
functions on very hot paths can avoid frames even when they do return
futures.

## Errors

Inside a coroutine, exceptions work as you would expect. If a coroutine
throws an exception and doesn't catch it, the exception doesn't escape to
its caller as it would from an ordinary function. Instead, it is stored
in the coroutine's future, which fails. When the caller `co_await`s a
failed future, `co_await` throws the exception again, in the caller, so
it can be caught with an ordinary `try` and `catch`.

> **C++ corner: no `co_await` in a `catch` block.** C++ forbids
> `co_await` inside a `catch` block, because a coroutine can't be
> suspended while an exception is being handled. So when handling an
> exception needs to wait, for example to send an error message to the
> client, the `catch` block can only record what happened, and the
> waiting must come after it. You will see this in Shoal below.

### The cost of exceptions

Exceptions are convenient, but they are slow, because throwing one
involves the C++ runtime's machinery for unwinding the stack. That
doesn't matter when they are rare. It does matter when they aren't: when
a server is overloaded, and many requests fail with timeouts, or when
clients send many bad requests. Then the cost of the exceptions makes
the overload worse. Large Seastar applications learned this the hard
way, and Seastar provides ways to report errors from coroutines without
throwing.

Here are two versions of a coroutine that parses a number. The first
throws when its input isn't a number:

```{.cpp include="examples/coroutines/errors.cc" snippet="throwing"}
```

The second puts the exception in its future without throwing it:

```{.cpp include="examples/coroutines/errors.cc" snippet="returning"}
```

`coroutine::return_exception()` ends the coroutine at once, and its
future fails with the given exception, which is never thrown.
(`coroutine::return_exception_ptr()` does the same with an exception you
already hold in a `std::exception_ptr`.)

On the receiving side, the usual way is to catch the exception:

```{.cpp include="examples/coroutines/errors.cc" snippet="catching"}
```

But `co_await` throws the exception it finds in the future, so even if
`parse` didn't throw, this catches a thrown exception.
`coroutine::as_future()` avoids that. Awaiting it gives back the future
itself, available or failed, so the coroutine can check it without any
exception being thrown:

```{.cpp include="examples/coroutines/errors.cc" snippet="checking"}
```

(`ignore_ready_future()` tells Seastar that we deliberately dropped the
failed future's exception; otherwise, it would warn about an exception
that nobody handled.) A third tool, `coroutine::try_future()`, is for a
coroutine that just wants to pass errors on to its own caller: if the
awaited future failed, the coroutine ends at once, with the same failure,
again without throwing.

The program that contains these functions measures each combination a
million times. On the laptop of [chapter](#ch:tutorial-introduction),
with its Intel Core Ultra 7 258V:

| The callee… | The caller… | Time per call |
|---|---|---|
| succeeds | | 10 ns |
| throws | catches | 1,120 ns |
| uses `return_exception()` | catches | 770 ns |
| uses `return_exception()` | uses `as_future()` | 48 ns |

A failure that is never thrown costs about the same as a few memory
allocations; one that is thrown and caught costs twenty times more. This
doesn't mean you should avoid exceptions everywhere: for errors that are
rare, the simplicity of `throw` and `catch` is worth it, and that is what
most of this book uses. But on a path where errors may be frequent, and
especially where they become frequent exactly when the server is
overloaded, use the tools above. [Chapter](#ch:errors-and-shutdown) goes
further.

## Reading and writing

[Chapter](#ch:tutorial-introduction) showed an echo server, which reads
from a connection and writes back. Let's look more closely at the types
it used.

A `connected_socket` is a network connection. Its `input()` method
creates an `input_stream<char>`, from which the program reads what the
client sends, and `output()` creates an `output_stream<char>`, to which
it writes. Each call creates a new stream object, and each stream has a
buffer of its own, so a program creates them once per connection, keeps
them in variables, and uses those same objects throughout:

- The input stream reads ahead. It may receive more than the program
  asks for, such as the beginning of the client's next request, and keeps
  the rest in its buffer for the next read. A second input stream on the
  same connection wouldn't see those bytes.
- The output stream collects what the program writes in its buffer, and
  sends it when the buffer is full, or when the program calls `flush()`
  or `close()`. So writing with `s.output().write(...)` would put the data
  in the buffer of a temporary stream, which C++ destroys right away,
  with the data never sent.

Both streams must also stay alive as long as a coroutine uses them,
across all its `co_await`s. Here that's easy, because they are local
variables of the coroutine, which live in its frame. In general, keeping
objects alive while asynchronous code uses them is one of the main
difficulties of asynchronous programming, and the subject of
[chapter](#ch:object-lifetime).

The input stream's `read()` returns whatever data has arrived, in a
`temporary_buffer<char>`, or an empty buffer when the client has closed
the connection. `read_exactly(n)` waits until it has `n` bytes, and
returns fewer only if the connection was closed. A `temporary_buffer` is
a piece of memory that the buffer owns and frees when it's destroyed, and
it is designed to avoid copying: it can be moved but not implicitly
copied, its `trim_front()` drops bytes from its beginning without
moving the rest, and `share()` makes another `temporary_buffer` for part
of the same memory. Seastar's network code fills such buffers directly,
and Shoal uses them the same way: it keeps the data it hasn't used yet in
a `temporary_buffer`.

The output stream's `write()` accepts strings and buffers, and returns a
`future<>`: usually it's available immediately, after copying the data
into the stream's buffer, but when the buffer is full and the network is
slow, it waits. `flush()` sends what the buffer holds, and `close()`
sends the rest and closes the connection.

Shoal keeps strings in `sstring`, Seastar's own string type. It works
much like `std::string`, and converts to and from it and from
`std::string_view`, but it was designed for Seastar's needs: it stores
strings of up to 15 bytes inside the object itself, without allocating
memory, which is common for the short keys and values of a server like
Shoal, and it can be created without initializing its contents, when the
program is about to fill them anyway, as Shoal does when it reads a
value from the network.

## Shoal, version 1

We now have everything we need to make Shoal a server.[^shoal-v1] Version
1 understands six Redis commands: `PING`, `ECHO`, `GET`, `SET`, `DEL` and
`EXISTS`. It keeps its data in memory, on a single shard, and it serves
many clients at once. That is enough for the standard Redis tools to
work with it.

[^shoal-v1]: The source of this version is in `shoal/v1/`.

Version 1 is too large for one file, so it has four, and a small header
that they all include. The header holds what every file needs, starting
with the `using` directive that [chapter](#ch:getting-started)
discussed:

```{.cpp include="shoal/v1/shoal.hh" snippet="using"}
```

and `CMakeLists.txt` lists the new source files:

```{.cmake include="shoal/v1/CMakeLists.txt" snippet="cmake"}
```

### The Redis protocol

Redis clients and servers talk in a simple protocol called RESP. A client
sends each request as an *array* of strings: the command's name, followed
by its arguments. The array starts with a line holding `*` and the number
of strings, and each string is a line holding `$` and its length,
followed by the string itself and a line ending. So the request `SET
greeting hello` is sent as:

```
*3\r\n$3\r\nSET\r\n$8\r\ngreeting\r\n$5\r\nhello\r\n
```

where `\r\n` is a carriage return and a line feed, the line ending that
the protocol uses. Because every string comes with its length, a value
may contain any bytes at all, line endings included. Redis servers also
accept requests typed by people, with tools like `nc`: a single line of
words separated by spaces. Redis calls them *inline* requests.

Replies are similar, with a different first character for each type: a
*simple string* such as `+OK\r\n`, an *error* such as `-ERR unknown
command\r\n`, an *integer* such as `:2\r\n`, a *bulk string*, which
carries its length like the strings of a request, such as
`$5\r\nhello\r\n`, and the *null* bulk string `$-1\r\n`, which `GET`
returns for a key that doesn't exist. Shoal formats them with five small
functions:

```{.cpp include="shoal/v1/resp.cc" snippet="replies"}
```

### Reading requests

A request is a `std::vector` of strings, and a client that breaks the
protocol causes a `protocol_error`:

```{.cpp include="shoal/v1/resp.hh" snippet="request"}
```

The `request_reader` reads requests from a client's input stream. It
holds a reference to the stream, which belongs to the coroutine that
serves the client, and a buffer of data that it has read but not yet
used:

```{.cpp include="shoal/v1/resp.hh" snippet="reader"}
```

You have already seen `read_more()` and `read_line()`. `read()` reads a
whole request:

```{.cpp include="shoal/v1/resp.cc" snippet="read"}
```

When a request breaks the protocol, `read()` and the functions it calls
fail with `coroutine::return_exception()`, so that a client sending
garbage doesn't make Shoal pay for an exception at every step. The limits
on the number of arguments and on their sizes are Redis's, and they
protect Shoal from a client that announces a string of a petabyte.
`read_exactly()`, not shown, copies what it needs from the buffer, and
reads the rest of a long string directly from the stream, with the
stream's own `read_exactly()`.

### Executing commands

Shoal stores its data in a `std::unordered_map` from keys to values, and
executes requests with an ordinary function:

```{.cpp include="shoal/v1/commands.hh" snippet="store"}
```

Each command is a small function that takes the store and the request,
and returns the reply:

```{.cpp include="shoal/v1/commands.cc" snippet="commands"}
```

A table maps each command's name to its function, and to the number of
words its requests must have, which Redis calls its *arity*:

```{.cpp include="shoal/v1/commands.cc" snippet="table"}
```

`execute()` looks the command up, checks the number of arguments, and
calls it. Redis command names are case-insensitive, so it converts the
name to lowercase first:

```{.cpp include="shoal/v1/commands.cc" snippet="execute"}
```

The error messages are the same as Redis's, so clients and tools that
look at them see what they expect.

### Serving clients

The coroutine that serves a client puts these pieces together:

```{.cpp include="shoal/v1/main.cc" snippet="handle-connection"}
```

It reads requests until the client closes the connection, executes each
one, writes the reply, and flushes it. Flushing after every reply looks
wasteful. Redis clients may *pipeline* their requests: send many of them
at once, and only then read the replies. Sending each reply in a network
packet of its own, with a system call of its own, would be slow. But on a
network connection, Seastar's `flush()` doesn't send anything right away.
It marks the stream as needing a flush, and returns at once; the reactor
sends the data of all the marked streams together, after running the
tasks that are ready. So when a client sends a batch of requests, Shoal
executes them one after the other, with each reply waiting in the
stream's buffer, and they all leave together. Exercise 3 shows how
effective this is.

The first line of `handle_connection()` is just as important.
`set_nodelay(true)` turns off *Nagle's algorithm*, a feature of TCP that
delays sending a small packet while an earlier one hasn't been
acknowledged yet, hoping to combine it with more data. Many clients
delay their acknowledgments too, by up to 40 milliseconds, hoping to
combine them with data of their own. When a request arrives in more than
one piece, Shoal sends the replies to the first piece, and then the
client waits for the rest, while the two algorithms wait for each other.
Without `set_nodelay(true)`, a test program that sent Shoal pipelines of
a thousand `APPEND` requests got the replies to each pipeline about 47
milliseconds late, which made it fifteen times slower. Redis and Valkey
turn off Nagle's algorithm on every connection, and a Seastar server
should too: Seastar leaves the choice to the application.

If a request breaks the protocol, the `catch` block records the error,
and, since it can't `co_await`, the error reply is written after it. Then
Shoal closes the connection, as Redis does, because after a protocol
error there is no way to know where the next request begins. Any other
exception, such as an error from the network when the client disappears,
isn't caught here, so it fails the coroutine's future.

Finally, `shoal_main()` creates the store, listens for clients, and
serves each one in the background, as the echo server of
[chapter](#ch:tutorial-introduction) did:

```{.cpp include="shoal/v1/main.cc" snippet="shoal-main"}
```

Version 1 keeps all its data in one store, on shard 0. So it listens with
the `fixed` load-balancing algorithm, which gives every connection to the
shard named by `fixed_cpu`, shard 0, however many shards Seastar runs.
As [chapter](#ch:tutorial-introduction) showed, the default algorithm
would give connections to shards that don't listen, and those clients
would wait forever. [Part](#part:many-cores) makes Shoal use every shard.

The store is a local variable of `shoal_main()`, and the coroutines that
serve the clients use it through a reference. That works here, because
`shoal_main()` never returns, so the store lives as long as the program.
In general, a background coroutine that keeps a reference to an object
must not outlive it, a subject we return to in
[chapter](#ch:object-lifetime).

## Trying it out

Build version 1 as you built version 0, and run it, here on two shards
and on port 6390, with Shoal's debug messages:

```
$ ./build/shoal -c2 -m1G --port 6390 --logger-log-level shoal=debug
INFO  2026-11-06 17:20:35,212 [shard 0:main] shoal - Shoal listening on port 6390
```

In another terminal, use `valkey-cli`, Valkey's command-line client:

```
$ valkey-cli -p 6390 SET greeting hello
OK
$ valkey-cli -p 6390 GET greeting
"hello"
$ valkey-cli -p 6390 GET nothing
(nil)
$ valkey-cli -p 6390 EXISTS greeting nothing
(integer) 1
$ valkey-cli -p 6390 DEL greeting
(integer) 1
$ valkey-cli -p 6390 INCR counter
(error) ERR unknown command 'INCR'
```

Every command is a new connection, which Shoal logs:

```
DEBUG 2026-11-06 17:20:36,139 [shard 0:main] shoal - client 127.0.0.1:54420 connected
```

Run `valkey-cli -p 6390` without a command, and it lets you type commands
interactively.

### Tests

Shoal comes with a test suite, written in Python with `pytest` and the
`redis` client library (on Fedora, the packages `python3-pytest` and
`python3-redis`). It checks each command, including values with
arbitrary bytes and values of several megabytes, and also pipelining,
inline requests, protocol errors, and many clients at once. The script
`run`, next to the tests, starts a Shoal on a free port, runs the tests
against it, and stops it. By default it runs the Shoal built in
`shoal/v1/build`; `--shoal` gives a different one:

```
$ cd shoal/v1/test
$ ./run -q
...............                                                          [100%]
15 passed in 0.25s
```

(`-q`, and any other options that `run` doesn't know, are passed to
`pytest`. For example, `./run -v -k pipeline` runs, verbosely, only the
tests whose names contain "pipeline".)

The tests check what Redis does, not what Shoal happens to do, so they
can test any Redis-compatible server. Running them against a real one
checks the tests themselves: a test that fails against Valkey tests the
wrong thing. `./run --valkey` starts a `valkey-server` instead of Shoal:

```
$ ./run --valkey -q
...............                                                          [100%]
15 passed in 0.22s
```

Writing the tests this way caught a mistake in one of them, before it
could mislead us about Shoal: it failed against both servers, because
the `redis` library interprets the reply to `PING` in a way the test
hadn't expected. [Chapter](#ch:testing) says more about testing Shoal.

### How fast is it?

As in [chapter](#ch:tutorial-introduction), we measured Shoal with
`valkey-benchmark`, on the same laptop with an Intel Core Ultra 7 258V.
A benchmark measures the server only if the client can send more
requests than the server can answer, so this time we gave the client
every advantage. The server runs on one fast core, and `taskset` pins
it there, with all its threads, so the operating system can't move it to
a slower core; Shoal does that itself, with `--cpuset`. The client runs
on the other three fast cores, with two threads, 96 connections, and
pipelines of 128 requests, which makes each request cheaper for the
client to send:

```
$ ./build/shoal --cpuset 0 -m1G --port 6390
$ taskset -c 1-3 valkey-benchmark -p 6390 -t set,get -n 40000000 -P 128 -c 96 --threads 2 -q
```

and then the same with `valkey-server`, started with
`taskset -c 0 valkey-server --port 6391 --save ''`. The results were the
same, within 2%, in three runs:

| Server, on one core | `SET` per second | `GET` per second |
|---|---|---|
| `valkey-server` | 2,850,000 | 3,890,000 |
| Shoal version 1 | 3,550,000 | 4,210,000 |

This time the servers, not the client, were the limit: with the same
client, Valkey answered 5.3 million `GET`s per second when the keys
didn't exist, which takes less work. On one core, Shoal version 1 is
faster than Valkey, by a quarter for `SET`. That's not a fair contest,
because Valkey does more for each command than Shoal does, such as
keeping statistics and checking each key's expiration time. But it shows
that writing a server with coroutines and futures doesn't make it slow:
on one core, Shoal is in the same league as Valkey, a server written
carefully in C. The interesting comparison comes in
[Part](#part:many-cores), when Shoal uses every core, and Valkey still
executes its commands on one.

## Exercises

1. Add the `INCR` command: it adds 1 to the number stored at a key, or to
   0 if the key doesn't exist, and replies with the new value. If the
   value isn't an integer, Redis replies with the error `value is not an
   integer or out of range`. Add a test for it to Shoal's test suite, and
   check that the test passes against both Shoal and `valkey-server`.
   (Careful: the `redis` library's `incr()` sends the more general
   `INCRBY` command; send `INCR` with `execute_command('INCR', key)`, or
   implement `INCRBY` too.)

2. Add the `APPEND key value` command, which appends to the value at a
   key, or sets it if the key doesn't exist, and replies with the new
   length. Then, from Python, append a single byte to the same key
   100,000 times, and then a million times, sending the requests in
   pipelines of a thousand (`r.pipeline(transaction=False)` in the
   `redis` library). Time it against Shoal and against `valkey-server`.
   How does the time grow with the number of appends, and why? (Hint:
   what does appending to an `sstring` cost?)

3. Run `valkey-benchmark -t set` against Shoal twice, with `-P 16`
   (pipelines of 16 requests) and with `-P 1` (no pipelining). During each
   run, count Shoal's calls to `sendmsg`, the system call that sends data
   on the network, with `strace -f -c -e trace=sendmsg -p` and Shoal's
   process ID. How many requests does each call answer? Remember that
   Shoal calls `flush()` after every reply.

4. Run Shoal with `-c2 -m1G`, and send it a request that announces a
   string of 600 megabytes, `*2\r\n$3\r\nGET\r\n$600000000\r\n`, with
   `printf` and `nc`. What happens? Now announce a string of 500
   megabytes, which is under Redis's limit, and close the connection
   without sending it. What happens now? Look at Shoal's log. What does
   this say about trusting what a client announces?

5. In `errors.cc`, the coroutines that succeed cost 10 nanoseconds per
   call. Write a version of `parse_returning()` that isn't a coroutine:
   an ordinary function that returns `make_ready_future<int>(n)` on
   success, and `make_exception_future<int>(parse_error(...))` on failure.
   How much faster is it, on success and on failure? What does that say
   about coroutine frames?

6. Send a million `APPEND` requests in a *single* pipeline, to
   `valkey-server` and then to Shoal. What happens? While it happens,
   look at the connection with `ss -tn`: how much data waits in each
   direction? Why does Valkey finish, and Shoal not? (Hint: what does
   Shoal do while `out.write()` waits, and what does the client do?)
