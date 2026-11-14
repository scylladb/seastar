# A tutorial introduction {#ch:tutorial-introduction}

The fastest way to get a feel for a new framework is to write a few small
programs with it and run them. This chapter does exactly that. It starts
with a program that prints a greeting, and ends a few pages later with a
network server that uses every core of the machine, which we then measure.
Along the way you will meet most of the ideas that the rest of the book
develops: shards, futures and coroutines, concurrency without threads,
and network I/O.

We will move fast, and skip many details on purpose. Every idea in this
chapter comes back later, in a chapter of its own, with the details and
the pitfalls this chapter glosses over. Here, the goal is only to see what
Seastar programs look like, and to watch them run.

To build the programs in this chapter, you need Seastar itself, built and
installed. [Chapter](#ch:getting-started) explains how to get it and
build it, and [appendix](#app:building-seastar) covers building it in
more detail. All the programs in this chapter are complete. You can find
them in the book's source, in `examples/tutorial-introduction/`, and the
text shows them, or the interesting parts of them, exactly as they are
there.

## Hello, world

Here is the first Seastar program:

```{.cpp include="examples/tutorial-introduction/hello.cc"}
```

Every Seastar program starts like this. The `main()` function creates an
`app_template`. Its `run()` method parses the command line, starts
Seastar, and then calls the function we give it, here a lambda. When the
work of this function is finished, Seastar shuts down, and `run()`
returns the program's exit code.

Everything that Seastar provides is in the `seastar` namespace, so the
full name of `app_template` is `seastar::app_template`. To keep the code
short, `hello.cc` starts with `using namespace seastar;`, and then uses
Seastar's names without the prefix. All the programs in this book do
the same, and so does ScyllaDB. [Chapter](#ch:getting-started) compares this with the
other ways that Seastar applications deal with the namespace.

The lambda is a [*coroutine*]{index="coroutine" .main}: a function that can suspend itself, and
resume later. Coroutines were added to C++ in C++20, with three new
keywords: []{index="`co_await`"}`co_await`, which suspends the coroutine until something it
waits for is ready, []{index="`co_return`"}`co_return`, which returns from a coroutine, and
[]{index="`co_yield`"}`co_yield`, for coroutines that produce a series of values, which we
won't need for a while. A function that uses any of them is a coroutine.
Our lambda has nothing to wait for: it prints its greeting and ends with
`co_return`, which is what makes it a coroutine. What it returns is a
`future<>`, an object that stands for the end of work that may not have
happened yet; that's how `run()` learns when the lambda's work is
finished. We call a function that returns a future an
[*asynchronous function*]{index="asynchronous function" .main}: when it returns, its work may not be finished yet, and the
future tells when it is. We will make more interesting use of coroutines
and futures in a moment.

The compiler needs help to compile a coroutine: it must know how a
coroutine that returns a `future<>` behaves. Seastar provides that in
[`<seastar/core/coroutine.hh>`]{index="coroutine!header needed for"}, so every file that defines a coroutine
must include this header, as `hello.cc` does. Without it, the compiler's
error message is confusing: it says that it can't find
`std::coroutine_traits`, the template through which C++ finds this
knowledge, and Clang even suggests including `<coroutine>`, which isn't
enough. Each part of Seastar has a header of its own, and this book
names it where it introduces that part; the complete source of every
example, in the book's repository, shows all the headers that it
includes.

Compile it with a single command:

```
$ g++ -O2 hello.cc -o hello $(pkg-config --cflags --libs --static seastar)
```

[`pkg-config`]{index="`pkg-config`" .main} supplies everything the compiler and linker need: the
location of Seastar's headers, the C++ standard Seastar was built with,
and the libraries a Seastar program needs, Seastar's own and those it
depends on. The `--static` option is there because Seastar is normally
built as a static library, so the program must also link with the
libraries that Seastar uses. Use the same compiler that built Seastar:
`g++` here, or `clang++` if your Seastar was built with Clang, because
some of the flags that `pkg-config` gives are specific to one compiler.
[Chapter](#ch:getting-started) explains how to choose. Compiling even
this small program takes a few seconds, because Seastar's headers are
large.

Then run it:

```
$ ./hello
INFO  2026-11-06 14:41:11,070 seastar - Reactor backend: io_uring
INFO  2026-11-06 14:41:11,308 seastar - Perf-based stall detector creation failed (EACCESS), try setting /proc/sys/kernel/perf_event_paranoid to 1 or less to enable kernel backtraces: falling back to posix timer.
INFO  2026-11-06 14:41:11,309 [shard 4:main] io - IO queue was unable to find a suitable maximum request length, the search was cut-off early at: 16MB
INFO  2026-11-06 14:41:11,309 [shard 4:main] io - IO queue was unable to find a suitable maximum request length, the search was cut-off early at: 16MB
Hello, world
```

Before our greeting, Seastar logs a few lines about how it set itself up:
which kernel interface it uses to wait for I/O, how it detects code that
runs too long, and what it found out about the disk, once for writes
and once for reads. You can ignore them
for now: [chapter](#ch:how-seastar-runs-your-code) explains the first
two, and [chapter](#ch:io-scheduler) the lines about the disk. The rest
of this chapter leaves them out of the outputs it shows.

Small as it is, this program already did something unusual. By default,
Seastar takes over the whole machine: it starts a thread on every core,
and takes almost all of the machine's memory, dividing it between the
cores. That is what a server that owns the machine wants, but for
experiments it's often too much. Two options control it:
[`-c`]{index="command-line options!`-c`, `--smp`"} (or
`--smp`) sets the number of cores to use, and
[`-m`]{index="command-line options!`-m`, `--memory`"} (or `--memory`) the
amount of memory for the whole program, which Seastar divides equally
between the cores. So this command uses only two cores and one gigabyte
of memory, half a gigabyte for each core:

```
$ ./hello -c2 -m1G
```

We recommend giving both options whenever you run an experiment on a
computer that you also use for other things, and the rest of this book
does so. `./hello --help` lists all of Seastar's options; there are many,
and [appendix](#app:command-line-options) explains them.

## Every core says hello

Seastar calls the thread that runs on each core, together with the part
of the memory that belongs to it, a [*shard*]{.index}. The shards are numbered from
0, and the function we passed to `run()` runs on shard 0. To use the other
shards, we have to ask for it explicitly. This program asks every shard to
say hello. The interesting part of it, inside the lambda, is:

```{.cpp include="examples/tutorial-introduction/shards.cc" snippet="hello-all"}
```

`this_smp_shard_count()` returns the number of shards, and
`this_shard_id()` the number of the shard that calls it.
`smp::invoke_on_all()` runs a function on every shard, and returns a
future that is resolved when all of them have finished. All three come
from `<seastar/core/smp.hh>`. Our coroutine
waits for it with `co_await`, so the program ends only after every shard
has said hello. Here is the program on four shards, run twice:

```
$ ./shards -c4 -m1G
Seastar is running 4 shards
Hello from shard 0
Hello from shard 1
Hello from shard 2
Hello from shard 3
$ ./shards -c4 -m1G
Seastar is running 4 shards
Hello from shard 0
Hello from shard 1
Hello from shard 3
Hello from shard 2
```

As you can see, the shards didn't greet in order, and the order changes
from run to run, because the four shards really do run at the same time,
each on its own core, and nothing decides which of them prints first. This is the only kind of [parallelism]{.index}
in a Seastar program: one thread per core, each working on its own. Code
running on one shard never runs in parallel with other code on the same
shard.

## Waiting without blocking

So far, nothing in our programs had to wait. Server programs wait all the
time: for a client's request to arrive, for the disk, for a timer. Let's
see what waiting looks like in Seastar, with a function that counts slowly:

```{.cpp include="examples/tutorial-introduction/sleep.cc" snippet="count"}
```

`count()` is a coroutine too: it returns a `future<>`, which is resolved
when it has finished counting. Each `co_await sleep(interval)` suspends the
coroutine for the given time. It doesn't block the thread: while the
coroutine sleeps, the shard is free to run other code, and Seastar resumes
the coroutine when the time is up.

This line looks just like an ordinary call to a sleep function, but the
difference is crucial. `seastar::sleep()`, from `<seastar/core/sleep.hh>`,
returns a future at once, and
`co_await` suspends only this coroutine. A traditional sleep, such as
`std::this_thread::sleep_for()` or the C library's `sleep()`, would
instead block the whole thread, and with it the whole shard: for the
entire interval, nothing else could run on that core, and every other
coroutine and every client of the shard would wait. In Seastar code,
always wait with `co_await`, never with a call that [blocks]{index="blocking"}.
[Chapter](#ch:how-seastar-runs-your-code) measures what a blocking call
does to a shard: while one coroutine called `std::this_thread::sleep_for()`
for a second, a timer that should have fired every millisecond waited the
whole second.

First, let's call `count()` twice, one call after the other, and measure
how long that takes:

```{.cpp include="examples/tutorial-introduction/sleep.cc" snippet="one-after-another"}
```

The program prints:

```
$ ./sleep -c1 -m1G
tick 1
tick 2
tick 3
tock 1
tock 2
one after the other: 1900ms
```

As expected, the second call starts only when the first has finished,
so the two take 3 × 300 + 2 × 500 = 1,900 milliseconds.

Then, let's run the same two calls at the same time.
`coroutine::all()` starts all the functions it is given, and waits until
all of them have finished:

```{.cpp include="examples/tutorial-introduction/sleep.cc" snippet="together"}
```

This part of the program prints:

```
tick 1
tock 1
tick 2
tick 3
tock 2
together: 1000ms
```

Now the ticks and tocks interleave, and both calls end after 1,000
milliseconds, the time of the longer one ([Figure](#fig:sleep-timeline)). We ran this program on a
single shard (`-c1`), so there was only one thread: the two counts waited
at the same time not because they ran on different threads, but because
neither of them held the thread while it waited. This is [concurrency]{.index}
without threads, and it's how a Seastar server serves many clients on
each core: each client is served by a coroutine like these, and while one
waits, the others run.

![The two runs of `sleep.cc` on a timeline. A bar is a sleep, and a dot is a
line printed. Either way, the thread runs code only for a moment at each
dark mark, and is free the rest of the time; run together, the two counts
wait in the same free time, and finish in 1,000 milliseconds instead of
1,900.](figures/sleep-timeline.svg){#fig:sleep-timeline}

## An echo server {index="echo server"}

Now for a real server. An *echo server* sends back to each client
whatever the client sends it. It is the "hello, world" of network
programming: it has all the parts of a server, and almost no logic of its
own. Here is how it serves one client:

```{.cpp include="examples/tutorial-introduction/echo.cc" snippet="handle-connection"}
```

A `connected_socket` represents a network connection. `input()` and
`output()` give us two streams over it: one to read what the client sends,
the other to write back to it. `in.read()` doesn't wait for data itself:
it returns at once, with a `future<temporary_buffer<char>>`, a future
that will hold a buffer of the bytes that the client sends next, once
they arrive. `co_await` waits for this future, and gives us the buffer
itself. An empty buffer means the client has closed the connection, so
we stop. Otherwise, `out.write()` sends the buffer back.
The output stream collects what we write in a buffer of its own, and
`out.flush()` sends it on its way. When the client is gone,
`out.close()` closes our side of the connection, too.

Each `co_await` is a point where the coroutine may have to wait: for the
client to send more, or for the reply to be sent. And while it waits,
the shard serves other clients. Yet `handle_connection()` reads like
ordinary, blocking code, from top to bottom.

The rest of the server accepts connections and hands each one to
`handle_connection()`:

```{.cpp include="examples/tutorial-introduction/echo.cc" snippet="serve"}
```

`listen()`, from `<seastar/core/seastar.hh>`, starts listening for
connections on the given port, on all of the machine's network addresses
(the types of sockets and their options come from
`<seastar/net/api.hh>`). Setting `reuse_address` lets us restart
the server immediately after stopping it, without waiting for the kernel to
release the port. Then a loop accepts the connections, one at a time:
`accept()` waits for the next one, and gives us its socket and the
client's address. We log the connection with `applog`, an object of
Seastar's `logger` class, defined just before `serve()`. A logger works
like `std::println()`, but adds the time, the shard and the logger's
name, and lets the user choose at run time how much to log.

The most interesting line of the loop is the call to
`handle_connection()`, which we don't `co_await`: if we did, the loop
would wait until this client disconnected before accepting the next one,
and the server would serve only one client at a time. Instead, the
connection is served in the background, while the loop goes back to
accepting more. The `(void)` says that we deliberately [drop the future]{index="`seastar::future`!dropping"}
that `handle_connection()` returns. The compiler would otherwise warn
us, because dropping a future is usually a mistake. But before dropping
it, we call its `handle_exception()` method, with a function that logs
the error, so that if serving the connection fails, the error isn't
silently lost ([chapter](#ch:concurrency-within-a-shard) explains why).
The function gets the exception as a `std::exception_ptr`, and
`formattable()` lets the logger print it, with its type and its message
([chapter](#ch:errors)).
Running [work in the background]{index="background work"} like this is common, and it has
consequences that we skip here: for example, nothing waits for these
background coroutines before the program exits.
[Chapter](#ch:concurrency-within-a-shard) and
[chapter](#ch:clean-shutdown) show how to keep track of them.

Finally, `main()`:

```{.cpp include="examples/tutorial-introduction/echo.cc" snippet="main"}
```

As before, `app.run()` calls a lambda, which here starts the server by
returning `serve(1234)`. The lambda doesn't need to be a coroutine: it
doesn't wait for anything itself, but passes on the future that
`serve()` returns, and `run()` waits for that future.

The three lines that create the `app_template` deserve an explanation.
By default, Seastar catches the [signals]{index="signals"} that ask a program to stop,
`SIGINT` (sent by Ctrl-C) and `SIGTERM` (sent by `kill`), and starts an
orderly shutdown, which waits for the function given to `run()` to
finish. Our `serve()` never finishes, so without these lines, Ctrl-C
would not stop the server, and neither would a plain `kill`. Setting
`auto_handle_sigint_sigterm` to false tells Seastar to leave these
signals alone, so they simply end the program. A real server catches
them instead, and shuts down cleanly, as
[chapter](#ch:clean-shutdown) explains.

Let's try it. In one terminal, start the server on a single shard. This
version of the server works correctly only with `-c1`, for a reason we
will see in a moment:

```
$ ./echo -c1 -m1G
```

In another, connect to it with `nc` (netcat), type a line, and see it come
back:

```
$ nc localhost 1234
hello
hello
world
world
```

And in the first terminal, the server logs the connection:

```
INFO  2026-11-06 15:17:58,280 [shard 0:main] echo - shard 0 serves 127.0.0.1:40868
```

You can connect many clients at once, from more terminals, and the server
serves them all, on its single thread.

We started the server with `-c1` for a reason. Try it without, so that it
starts a shard on every core, and connect several clients at once, and
most of them get no answer. When a new connection arrives, Seastar picks
the shard that will serve it, and moves the connection there, to spread
the clients evenly over the cores. But only shard 0 called `listen()` and
`accept()`, so connections given to the other shards wait forever for
someone to accept them. When we tried eight clients at once on four
shards, only one of them was served. This is a known pitfall (Seastar
issue 2183): a Seastar server must [listen on every shard]{index="`listen()`!on every shard"}. That's exactly
what we want to do, so let's see how.

## Using every core

To serve on every shard, `main()` runs `serve()` on all of them, with the
`smp::invoke_on_all()` we met before:

```{.cpp include="examples/tutorial-introduction/echo-all-cores.cc" snippet="main"}
```

Nothing else changes. All the shards now listen on port 1234, and each
runs its own accept loop, serving the connections Seastar gives it. Here
is what the server logs when eight clients connect at once to the server
running on four shards:

```
INFO  [shard 0:main] echo - shard 0 serves 127.0.0.1:49716
INFO  [shard 0:main] echo - shard 0 serves 127.0.0.1:49732
INFO  [shard 2:main] echo - shard 2 serves 127.0.0.1:49722
INFO  [shard 2:main] echo - shard 2 serves 127.0.0.1:49746
INFO  [shard 3:main] echo - shard 3 serves 127.0.0.1:49724
INFO  [shard 3:main] echo - shard 3 serves 127.0.0.1:49762
INFO  [shard 1:main] echo - shard 1 serves 127.0.0.1:49720
INFO  [shard 1:main] echo - shard 1 serves 127.0.0.1:49738
```

(We removed the date and time from these lines, such as
`2026-11-06 15:17:58,280` in the server's earlier log line, to make them
fit; the rest of the book does the same.) Each shard got
two of the eight clients. Each client is served by a single shard from
beginning to end, and the shards share nothing: there are no locks in this
program, and no data that two cores touch. A server with real data to
serve has more work to do, because a request may need data that belongs to
another shard. That is the subject of [Part](#part:many-cores).

## Measuring it {index="benchmarking"}

How fast is this server? To measure a server, we need a client that sends
it many requests, from many connections at once, and measures how many it
answers per second. Writing such a client is a project of its own, so
let's borrow one: [`valkey-benchmark`]{index="`valkey-benchmark`" .main}, the benchmark tool that comes with
the [Valkey]{.index} key-value store (and is also known by its original name,
`redis-benchmark`). Many Linux distributions package it.

> **About Redis and Valkey.** Valkey is a fork of Redis, made in 2024
> when Redis changed its license, and for everything in this book the two
> behave the same. So this book says "Redis" for the protocol and for
> what both do, and "Valkey" for the programs we run, and for what is
> Valkey's own.

`valkey-benchmark` speaks the
Redis protocol, so it can't measure an echo server, whose replies aren't
valid Redis replies. But it can measure a server that answers the Redis
`PING` command, whose reply is the line `+PONG`. A one-line change, well,
almost, turns our echo server into such a server. Instead of writing back
what it received, it writes `+PONG` for every line it received:

```{.cpp include="examples/tutorial-introduction/pong.cc" snippet="pong"}
```

The new program, `pong`, listens on port 6380, and doesn't log each
connection, so that logging doesn't slow it down. We measured it on a
laptop with an [Intel Core Ultra 7 258V]{index="measurements, machine used for"} processor, which has eight cores:
four fast ones and four slower ones, running Fedora 44, with GCC 16.
Unless they say otherwise, all the measurements in this book were made
on this laptop.

A benchmark measures the server only if the client can send more than
the server can answer, and the client needs cores of its own for that.
So we ran `pong` on one fast core, CPU 0, and the client on the other
three fast ones:

```
$ ./pong --cpuset 0 -m1G
$ taskset -c 1-3 valkey-benchmark -p 6380 -t ping_inline -n 400000000 -P 128 -c 96 --threads 2 -q
PING_INLINE: 12995451.00 requests per second, p50=0.471 msec
```

[`--cpuset`]{index="command-line options!`--cpuset`"} is like `-c`, but chooses which cores Seastar uses, and
`taskset` does the same for any other program. The client's options ask
for 400 million `PING` requests, over 96 connections, from 2 threads, with
up to 128 requests sent on each connection before waiting for their
replies. So many requests make the run last half a minute, which
matters: with `--threads`, `valkey-benchmark` notices that a run has
ended only at its next tick of a quarter of a second, so it rounds a
run's time up to a multiple of 0.25 seconds, and a run of a few seconds
would report a rate several percent too low. Sending requests without waiting for the previous replies is
called [*pipelining*]{.index .main}, and it makes each request much cheaper for both the
client and the server. Then we did the same with `valkey-server`, which
also answers `PING`, and also runs its commands on one thread, pinned to
the same core with `taskset -c 0 valkey-server --port 6391 --save ''`:

| Server, on one core | `PING`s per second |
|---|---|
| `valkey-server` | 4,880,000 |
| `pong` | 13,100,000 |

These are the best of three runs, which differed by 2% at most. Don't read too much into the
difference: Valkey does real work for each `PING`, it parses the
command, looks it up, and prepares the reply, while our server only
counts newlines. But it shows how little Seastar adds to the work a
server does: answering a request took `pong`, on average, about 76
nanoseconds. Giving `pong` a second core, one of the slower ones, raised
it to 15.0 million `PING`s per second, but not to twice as many. To
measure how much faster a Seastar server becomes with more cores, we
need a server that does more work for each request, so that the client
can keep it busy. Shoal is such a server, from [chapter](#ch:coroutines)
on, and [Part](#part:many-cores) measures how it speeds up with more
cores.

## What we skipped

Our programs work, but to keep them short, we skipped many things that
real programs need, and that the rest of the book covers:

- **What a future really is**, how coroutines work, and what they cost:
  [chapter](#ch:coroutines) and [chapter](#ch:futures-and-promises).
- **How Seastar runs your code** on each shard, and why code that never
  waits can still cause trouble: [chapter](#ch:how-seastar-runs-your-code).
- **Background work**, like our connections, and how to keep track of it:
  [chapter](#ch:concurrency-within-a-shard).
- **Object lifetimes**: our streams must live as long as a coroutine uses
  them, which is easy here and much harder in general:
  [chapter](#ch:object-lifetime).
- **Errors**, and what Seastar offers to handle them:
  [chapter](#ch:errors).
- **Clean shutdown**, instead of letting Ctrl-C kill the program:
  [chapter](#ch:clean-shutdown).
- **Shards that work together**, on data that belongs to one of them, and
  the `sharded<>` template that real servers use to run a service on every
  shard: [Part](#part:many-cores).
- **Networking in depth**: reading a protocol from a stream, writing large
  replies, and handling clients that send faster than we process:
  [chapter](#ch:networking).

The next chapter begins this work properly, with getting Seastar and
starting the program that we will build throughout the book.

## Exercises

1. Add a long `sleep()` to `hello`, so it doesn't end at once, and run it
   with `-c1`, `-c2` and with no `-c` at all. While it runs, list its
   threads, with `ps -L -p` followed by its process ID, or in `top` after
   pressing `H`. How many threads does it start in each case? Why are
   there more threads than shards? The threads' names are a hint.

2. Change `sleep.cc` to run three counts at the same time, and print the
   time each of them ends. Then run it with `-c4`. Does it run any faster
   with more shards? Why not?

3. The echo server writes back each buffer as soon as it reads it. Change
   it to echo whole lines only: collect what it reads until a newline
   arrives, then write back the line. (Hint: keep the incomplete line in a
   `std::string` between reads.) `nc` sends what you type a whole line at
   a time, so test it with a client that sends a line in pieces, such as
   `(printf hel; sleep 1; printf 'lo\n'; sleep 1) | nc localhost 1234`.

4. Run `echo-all-cores` with `-c4`, and connect to it with `nc` from four
   terminals, one after the other, closing each connection before opening
   the next. Which shards serve them? Now keep them all open. Which
   shards serve them now? The `lba` field of `listen_options` chooses how
   new connections are spread over the shards. Set it to
   `server_socket::load_balancing_algorithm::port`, and repeat the
   experiment. What decides the shard now? (`nc -p` chooses the port that
   the client connects from.)

5. Run `pong` and `valkey-benchmark` on your own machine as in the text,
   with pipelines of 1, 4, 16 and 128 requests (`-P`). How does the
   number of requests per second change? For each run, divide the number
   of requests that are waiting for a reply at any moment (the number of
   connections times the pipeline's depth) by the median time a request
   took, which `valkey-benchmark` reports as `p50`. What do you find?
