# Coroutines {#ch:coroutines}

[Chapter](#ch:tutorial-introduction) used coroutines without explaining
them, and you could follow it anyway: a coroutine reads almost like
ordinary code. This chapter explains what they are, how they wait without
blocking, how they call each other, how they report errors, and how they
read from and write to the network. Then we use all of that to turn Shoal
into a real server, which speaks the Redis protocol well enough that the
standard Redis tools work with it. This is Shoal's version 1.

Seastar wasn't designed around coroutines. When it was born, in 2014,
C++ had none, and Seastar's asynchronous code was written as chains of
[*continuations*]{index="continuation" .main}: lambdas, each called when the previous step
finished. Seastar added support for coroutines in 2019, even before C++20
made them part of the language, and today they are the normal way to
write Seastar code. Continuations are still useful, and you will see them
in older code, so [chapter](#ch:continuations) covers them; this book
starts with coroutines.

## Futures

A function that may have to wait, for data from the network, for the
disk, or for a timer, can't return its result directly, because the
result doesn't exist yet. Instead, it returns a [*future*]{index="`seastar::future`" .main}: an object that
stands for a result that will be available later. Such a function is what
[chapter](#ch:tutorial-introduction) called an [*asynchronous function*]{index="asynchronous function"}.
In Seastar, the type is `seastar::future<T>`, from
`<seastar/core/future.hh>`, where `T` is the type of the result. A function
that produces no result, but still takes time to finish, returns
`future<>`. For example, `seastar::sleep()` returns a `future<>` that
becomes available when the time is up, and reading from a network
connection returns a `future<temporary_buffer<char>>`, which will hold
the bytes read, in a buffer type that we meet later in this chapter.

A future is always in one of three [states]{index="`seastar::future`!states of" .main}. It may be *unavailable*: the
result isn't there yet. It may be *available*, holding the result. Or it
may be *failed*, holding an exception instead of a result, because the
operation failed. An unavailable future becomes available or failed
exactly once, and then stays that way.

Despite the similar name, a `seastar::future` is not a [`std::future`]{index="`std::future`"},
and the two don't mix: Seastar's functions return and take only
`seastar::future`, and a Seastar coroutine can't `co_await` a
`std::future`. A Seastar future is also much cheaper: it belongs to one
shard, so it needs no locks or atomic operations, and getting a result
that is already available costs almost nothing.

> **C++ corner: `std::future`.**[]{index="C++ corner!`std::future`"} C++11 added `std::future`, with
> `std::promise` and `std::async`, for passing a result from one thread
> to another. Its `get()` blocks the calling thread until the result
> arrives, which is exactly what a Seastar program must never do, and
> because two threads use it, it pays for synchronization between them.
> It has no way to run code when the result arrives: a `then()` method
> was proposed, and got as far as a Technical Specification in 2016,
> but never into the standard. So `std::future` has no place in a
> Seastar application: if you know it, don't reach for it, and if you
> don't, you don't need to learn it.

So how does a function use a future's result, if it can't wait for it?
It becomes a coroutine.

## Coroutines

A [*coroutine*]{index="coroutine" .main} is a function that can suspend itself in the middle, and be
resumed later from the same point. In C++, any function that uses one of
the keywords `co_await`, `co_return` or `co_yield` is a coroutine, and in
Seastar, a coroutine returns a `future`. Here is one. It reads from a
client's connection until the client closes its side, and returns the
number of bytes that the client sent:

```{.cpp include="examples/coroutines/count-bytes.cc" snippet="count-bytes"}
```

`in` is an `input_stream<char>`, from which a program reads what a
client sends, as the echo server of [chapter](#ch:tutorial-introduction)
did; the section *Reading and writing*, later in this chapter, explains
streams fully. `in.read()` reads from this stream: the
`future<temporary_buffer<char>>` it returns will hold the data that the
client sends next, when it arrives.

`co_await` waits for that future. If the data has already arrived, the
coroutine simply continues. If not, the coroutine [*suspends*]{index="coroutine!suspending"}: it returns to its caller at once, giving
it the coroutine's own future, which isn't available yet. The caller
usually waits for that future, so it suspends in the same way, and so
on, until control gets back to the reactor, which goes on to run other
work, such as serving other clients. When the data arrives,
Seastar resumes the coroutine where it stopped, and `co_await` gives it
the data. If the future failed, `co_await` throws its exception instead,
as we will see below.

`co_return` ends the coroutine and gives its result. The coroutine is
declared to return `future<size_t>`, but `co_return` gives a plain
`size_t`:
the future that the coroutine returned to its caller becomes available
with that value.

What makes this work is the coroutine's [*frame*]{index="coroutine!frame" .main}. When a coroutine is
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
is how larger asynchronous operations are built from smaller ones.
[Chapter](#ch:tutorial-introduction)'s tick and tock did this, calling
`count()` twice, and so does this coroutine, which tells a client how
many bytes it sent:

```{.cpp include="examples/coroutines/count-bytes.cc" snippet="report"}
```

Each of the two is a coroutine with its own frame. When
`count_bytes()` suspends, waiting for the network, `report()`, which is
waiting for it, is suspended too, at its own `co_await`, and when
`count_bytes()` returns the count, `report()` resumes with it. A chain
can be longer: in Shoal, later in this chapter, it is four coroutines
long. The code reads as if each call simply returned its result.

Note what this means: a coroutine is itself an asynchronous function,
since it returns a future. So a function that wants to use the result of
an asynchronous function must also be asynchronous: it must be a
coroutine that `co_await`s it, or return a future of its own in some
other way ([chapter](#ch:continuations) shows another). The one exception
is code that runs in a [*Seastar thread*]{index="Seastar thread"}, which can wait for a future
without being asynchronous itself ([chapter](#ch:threads)). An asynchronous
operation is built from smaller asynchronous functions, like
`report()` and `count_bytes()`, all the way down to the
asynchronous functions that Seastar provides, such as `sleep()` and
`read()`.

Creating a frame means allocating memory, which is cheap with Seastar's
memory allocator ([chapter](#ch:memory)), but not free, so not every
function should be a
coroutine. A function that never waits gains nothing from being one. In
Shoal, executing a command only looks up or changes a table in memory,
so `execute()`, which you will see below, is an ordinary function that
returns its reply directly. [Chapter](#ch:futures-and-promises) shows how
functions on very hot paths can avoid frames even when they do return
futures.

`count_bytes()` takes its stream by reference, and the stream is a local
variable of `report()`. That's safe here, because `report()` waits for
`count_bytes()` to finish before the stream goes away, but a reference
parameter of a coroutine deserves care:

> **C++ corner: coroutine parameters that are references.**[]{index="C++ corner!coroutine parameters that are references"}[]{index="coroutine!reference parameters"}
> A coroutine's frame holds copies of its parameters, but the copy of a
> reference is just another reference: the object it refers to stays
> where it was, and must outlive the coroutine. This is easy to get wrong
> with a temporary object, which C++ destroys at the end of the statement
> that created it. Take this coroutine:
>
> ```cpp
> future<> greet(const std::string& name) {
>     co_await sleep(10ms);
>     std::println("hello, {}", name);
> }
> ```
>
> `co_await greet("Shoal");` is safe: the temporary `std::string` made
> from `"Shoal"` lives until the end of the statement, and the statement
> includes the whole wait. But this is a bug:
>
> ```cpp
> auto f = greet("Shoal");
> co_await std::move(f);
> ```
>
> The temporary is destroyed at the end of the first line, while
> `greet()` sleeps, so when `greet()` wakes up, `name` refers to a string
> that no longer exists. When we ran it, it printed garbage instead of
> `Shoal`, and when built with GCC, it then crashed. So a coroutine should take its parameters by
> value, as `report()` takes its socket, unless every caller awaits it in
> the same statement, as `report()` awaits `count_bytes()`, or the object
> clearly outlives the coroutine. [Chapter](#ch:object-lifetime) returns
> to this, and to a similar trap with lambdas.

## Errors {index="exceptions"}

Inside a coroutine, exceptions work as you would expect. If a coroutine
throws an exception and doesn't catch it, the exception doesn't escape to
its caller as it would from an ordinary function. Instead, it is stored
in the coroutine's future, which fails. When the caller `co_await`s a
failed future, `co_await` throws the exception again, in the caller, so
it can be caught with an ordinary `try` and `catch`.

> **C++ corner: no `co_await` in a `catch` block.**[]{index="C++ corner!no `co_await` in a `catch` block"}[]{index="`co_await`!in a `catch` block"} C++ forbids
> `co_await` inside a `catch` block, because a coroutine can't be
> suspended while an exception is being handled. So when handling an
> exception needs to wait, for example to send an error message to the
> client, the `catch` block can only record what happened, and the
> waiting must come after it. You will see this in Shoal below.

### The cost of exceptions {index="exceptions!cost of"}

Exceptions are convenient, but they are slow, because throwing one
involves the C++ runtime's machinery for unwinding the stack. That
doesn't matter when they are rare. It does matter when they aren't: when
a server is overloaded, and many requests fail with timeouts. Then the
cost of the exceptions makes the overload worse. Large Seastar applications learned this the hard
way, and Seastar provides ways to report errors from coroutines without
throwing.

Here are two versions of a coroutine that parses a number. The first
throws when its input isn't a number:

```{.cpp include="examples/coroutines/errors.cc" snippet="throwing"}
```

The second puts the exception in its future without throwing it:

```{.cpp include="examples/coroutines/errors.cc" snippet="returning"}
```

`coroutine::return_exception()`, from
`<seastar/coroutine/exception.hh>`, ends the coroutine at once, and its
future fails with the given exception, which is never thrown.
(`coroutine::return_exception_ptr()` does the same with an exception you
already hold in a `std::exception_ptr`.)

> **C++ corner: `std::exception_ptr`.**[]{index="C++ corner!`std::exception_ptr`"}[]{index="`std::exception_ptr`" .main} An exception can be
> of any type, so code that keeps an exception for later, to pass it on
> or rethrow it, can't store it in a variable of the exception's own
> type. C++11 added `std::exception_ptr` for this: a pointer, with shared
> ownership, to an exception object of any type. In a `catch` block,
> `std::current_exception()` returns one that points to the exception
> being handled, and `std::make_exception_ptr(e)` makes one holding a
> copy of the exception object `e`. `std::rethrow_exception(p)` throws
> the exception again. Copying a `std::exception_ptr` doesn't copy the
> exception, and a default-constructed one, like a null pointer, holds no
> exception. A failed Seastar future keeps its exception in a
> `std::exception_ptr`. Until C++26, the type offered no way to look at
> the exception, or even ask what type it is, without rethrowing it; we
> come back to this below.

On the receiving side, the usual way is to catch the exception:

```{.cpp include="examples/coroutines/errors.cc" snippet="catching"}
```

But `co_await` throws the exception it finds in the future, so even if
`parse` didn't throw, this catches a thrown exception.
`coroutine::as_future()`, from `<seastar/coroutine/as_future.hh>`,
avoids that. Awaiting it gives back the future
itself, available or failed, so the coroutine can check it without any
exception being thrown:

```{.cpp include="examples/coroutines/errors.cc" snippet="checking"}
```

([`ignore_ready_future()`]{index="`seastar::future`!`ignore_ready_future()`"} tells Seastar that we deliberately dropped the
failed future's exception; otherwise, it would warn about an exception
that nobody handled.)

Unlike the `catch`, this counts every failure, not only a `parse_error`.
Checking the type of an exception without throwing it is harder than it
should be. `f.get_exception()` gives the exception, in a
`std::exception_ptr`, but before C++26, the only way to ask what is
inside one was to rethrow it and catch it, which is the very cost we
wanted to avoid. ScyllaDB, which needed this check on paths where errors
are frequent, wrote its own, `utils::try_catch<T>()`, which looks inside
the `std::exception_ptr` through the compiler's internal interfaces. C++26
added the same to the standard library, as [`std::exception_ptr_cast`]{index="`std::exception_ptr_cast`"}:
`std::exception_ptr_cast<parse_error>(e)` returns a pointer to the
exception if it is a `parse_error`, and a null pointer if it isn't,
without throwing. In C++26, the `if` in the loop above can therefore count
only the parse errors, and fail with any other error, still without
throwing:

```{.cpp include="examples/coroutines/errors.cc" snippet="checking-type"}
```

The program that contains these functions measures each combination a
million times. On the laptop of [chapter](#ch:tutorial-introduction),
with its Intel Core Ultra 7 258V, the fastest of three runs:

| The callee | The caller | Time per call |
|---|---|---|
| succeeds | | 10 ns |
| throws | catches | 810 ns |
| `return_exception()` | catches | 460 ns |
| `return_exception()` | `as_future()` | 47 ns |
| `return_exception()` | `as_future()`, rethrows to check the type | 480 ns |
| `return_exception()` | `as_future()`, `exception_ptr_cast` (C++26) | 48 ns |

A failure that is never thrown costs about the same as a few memory
allocations; one that is thrown and caught costs ten to seventeen times as
much. This
doesn't mean you should avoid exceptions everywhere: for errors that are
rare, the simplicity of `throw` and `catch` is worth it, and that is what
most of this book uses. But on a path where errors may be frequent, and
especially where they become frequent exactly when the server is
overloaded, use the tools above. [Chapter](#ch:errors)
shows what exceptions do to a server under overload, and another way to
report errors without them: result types, such as `std::expected`.

An error often has to pass through several coroutines on its way from
the one that fails to the one that handles it, and each of them would
normally rethrow it. Seastar's third tool for errors,
`coroutine::try_future()`, from `<seastar/coroutine/try_future.hh>`,
avoids that: if the awaited future failed, the
coroutine ends at once, with the same failure, without throwing. For
example, this coroutine parses a number and doubles it, and passes a
parse error on to its caller:

```{.cpp include="examples/coroutines/errors.cc" snippet="passing"}
```

Without `try_future()`, as `co_await parse_returning(s)`, it would pass the
error on too, but by throwing it: `co_await` would throw the exception,
and the coroutine would catch it and fail its own future with it. Here is
what each costs, measured as above, with a caller that uses `as_future()`:

| The coroutine passes the error on… | Time per call |
|---|---|
| with `co_await` | 500 ns |
| with `try_future()` | 57 ns |

> **Exceptions on many cores.**[]{index="exceptions!on many cores"} When Seastar was young, exceptions had a
> worse problem than their cost: they didn't scale. To unwind the stack,
> the C++ runtime must look up information about every function it
> passes, and on Linux, this lookup took a lock shared by the whole
> process. So threads that threw exceptions at the same time waited for
> each other. In a Seastar application, one shard throwing many
> exceptions slowed down the exceptions of all the others. Seastar worked
> around this in 2017 with a lookup of its own, without the lock. In
> 2022, Thomas Neumann's report to the C++ standards committee, P2544R0,
> *C++ exceptions are becoming more and more problematic*, said that this
> lock "has disastrous consequences for high core counts and makes
> exceptions nearly unusable on such machines", and a fix came out the
> same year, in glibc 2.35 and GCC 12. Today, exceptions
> thrown on different cores no longer wait for each other. Each one
> still costs about half a microsecond or more, as the table shows, so the advice
> above stands.

## Reading and writing {index="streams"}

[Chapter](#ch:tutorial-introduction) showed an echo server, which reads
from a connection and writes back. Here again is the coroutine that
served each connection:

```{.cpp include="examples/tutorial-introduction/echo.cc" snippet="handle-connection"}
```

Let's look more closely at the types it uses.

A [`connected_socket`]{index="`seastar::connected_socket`" .main}, here `s`, is a network connection. Its
[`input()`]{index="`seastar::connected_socket`!`input()`"} method creates an [`input_stream<char>`]{index="`seastar::input_stream`" .main}, from which
the program reads what the client sends, and [`output()`]{index="`seastar::connected_socket`!`output()`"} creates an
[`output_stream<char>`]{index="`seastar::output_stream`" .main}, to which
it writes; the two streams come from `<seastar/core/iostream.hh>`. Each call creates a new stream object, and each stream has a
buffer of its own, so a program must create them once per connection,
keep them in variables, here `in` and `out`, and use those same objects
throughout:

- The input stream reads ahead. It may receive more than the program
  asks for, such as the beginning of the client's next request, and keeps
  the rest in its buffer for the next read. A second input stream on the
  same connection wouldn't see those bytes.
- The output stream collects what the program writes in its buffer, and
  sends it when the buffer is full, or when the program calls `flush()`
  or `close()`. So naively writing with `s.output().write(...)` would put
  the data in the buffer of a temporary stream, which C++ destroys right
  away, before sending it. Seastar notices, and stops the program with
  the message "Was this stream properly closed?" Output streams are not
  the only Seastar objects that must be closed before they are
  destroyed; [chapter](#ch:clean-shutdown) lists the others, and
  what happens when you forget.

Both streams must also stay alive as long as a coroutine uses them,
across all its `co_await`s. Here that's easy, because they are local
variables of the coroutine, which live in its frame. In general, keeping
objects alive while asynchronous code uses them is one of the main
difficulties of asynchronous programming, and the subject of
[chapter](#ch:object-lifetime).

The input stream's [`read()`]{index="`seastar::input_stream`!`read()`"} waits until some data has arrived, and returns
it, in a `temporary_buffer<char>`, or an empty buffer when the client has
closed the connection. It promises nothing about how much data it
returns: whatever the stream has in its buffer, or else whatever the
connection delivers next, which may be part of a request, or several
requests. Another method, [`read_exactly(n)`]{index="`seastar::input_stream`!`read_exactly()`"}, waits until it has `n` bytes,
and returns fewer only if the connection was closed.

A [`temporary_buffer<T>`]{index="`seastar::temporary_buffer`" .main}, from
`<seastar/core/temporary_buffer.hh>`, is an object that owns a piece of memory, an
array of `T`, and frees it when it's destroyed. It is designed to avoid
copying: it can be moved but not implicitly copied, its
[`trim_front()`]{index="`seastar::temporary_buffer`!`trim_front()`"} drops elements from its beginning without moving the rest,
and [`share()`]{index="`seastar::temporary_buffer`!`share()`"} makes another buffer for part of the same memory. Seastar's
network code fills such buffers directly, and Shoal uses them the same
way: it keeps the data it hasn't used yet in a `temporary_buffer<char>`.

The output stream's [`write()`]{index="`seastar::output_stream`!`write()`"} accepts strings and buffers, and returns a
`future<>`: usually it's available immediately, after copying the data
into the stream's buffer, but when the buffer is full and the network is
slow, it waits. [`flush()`]{index="`seastar::output_stream`!`flush()`"} makes the stream send what its buffer holds, and [`close()`]{index="`seastar::output_stream`!`close()`"} sends the rest
and closes the connection.

## Shoal, version 1 {index="Shoal!version 1"}

We now have everything we need to make Shoal a server.[^shoal-v1] Version
1 understands six Redis commands: `PING`, `ECHO`, `GET`, `SET`, `DEL` and
`EXISTS`. It keeps its data in memory, on a single shard, and it serves
many clients at once. That is enough for the standard Redis tools to
work with it.

| Command | What it does | Reply |
|---|---|---|
| `PING [message]` | checks that the server answers | `PONG`, or the message |
| `ECHO message` | the same, with a message | the message |
| `SET key value` | stores the value under the key | `OK` |
| `GET key` | looks the key up | its value, or null if there is none |
| `DEL key [key ...]` | deletes the keys | how many keys it deleted |
| `EXISTS key [key ...]` | checks whether the keys exist | how many of them exist |

[^shoal-v1]: The source of this version is in `shoal/v1/`.

Version 1 is too large for one file, so it is split into several: the
Redis protocol (`resp.hh` and `resp.cc`), the commands (`commands.hh`
and `commands.cc`), the server itself (`main.cc`), and a small header that
they all include (`shoal.hh`). The header holds what every file needs, starting
with the `using` directive that [chapter](#ch:getting-started)
discussed:

```{.cpp include="shoal/v1/shoal.hh" snippet="using"}
```

and `CMakeLists.txt` lists the new source files:

```{.cmake include="shoal/v1/CMakeLists.txt" snippet="cmake"}
```

### The Redis protocol {index="RESP"}

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
words separated by spaces. Redis calls them [*inline* requests]{index="RESP!inline requests"}.

Replies are similar, with a different first character for each type: a
[]{index="RESP!data types"}*simple string* such as `+OK\r\n`, an *error* such as `-ERR unknown
command\r\n`, an *integer* such as `:2\r\n`, a *bulk string*, which
carries its length like the strings of a request, such as
`$5\r\nhello\r\n`, and the *null* bulk string `$-1\r\n`, which `GET`
returns for a key that doesn't exist. Shoal formats them with five small
functions:

```{.cpp include="shoal/v1/resp.cc" snippet="replies"}
```

### Reading requests

Shoal's type `request` is a `std::vector` of strings, and a client that
breaks the protocol causes Shoal's exception `protocol_error`:

```{.cpp include="shoal/v1/resp.hh" snippet="request"}
```

The strings are [`sstring`]{index="`seastar::sstring`" .main}s, Seastar's own string type, from
`<seastar/core/sstring.hh>`. An `sstring`
works much like a `std::string`, and converts to and from it and from
`std::string_view`. Shoal uses one of its extras: a constructor that
creates a string of a given length without initializing it,
`sstring(sstring::initialized_later(), n)`, for a value that Shoal is
about to fill from the network anyway. (`std::string` has nothing quite
like it. Since C++23, its `resize_and_overwrite()` also adds characters
without initializing them, but a function that you give it must fill
them before it returns, so they can't wait for the network, as Shoal's
do.)

> **Why Seastar has its own string type.**[]{index="`seastar::sstring`!history of"} When Seastar was born, in
> 2014, the `std::string` of [libstdc++]{.index}, the C++ library that comes with
> GCC, and that Clang usually uses too on Linux, was a poor fit for it. Copies of a string
> shared its memory, with a reference count that every copy and every
> destruction updated with an atomic operation, which is expensive on a
> machine with many cores, as [chapter](#ch:case-for-seastar) explained.
> And even a short string was allocated on the heap. So Seastar wrote
> `sstring`, which keeps strings of up to 14 characters inside the object
> itself, and gives every longer string memory of its own. C++11 had
> already forbidden strings that share memory, but libstdc++ kept its
> old string, for compatibility, until the version that came with GCC 5,
> in 2015. Today's `std::string`
> works much like `sstring`, and the two differ only in details:
>
> - An `sstring` takes up 16 bytes, and keeps up to 14 characters inside
>   the object, and the null that ends them, which, as with `std::string`,
>   makes `c_str()` free. The `std::string` of libstdc++ takes up 32 bytes, because it always
>   keeps a pointer to its characters and their length, even when the
>   characters are inside the object; the other 16 bytes hold up to 15
>   characters and the null that ends them. [libc++]{.index}, the C++
>   library of the LLVM project, packs its `std::string` tighter: 24 bytes, with up to 22
>   characters inside.
> - `sstring` has a few extras: the constructor above, `release()`,
>   which hands its memory over to a `temporary_buffer<char>` without
>   copying, and `make_sstring()`, which concatenates its arguments.
> - `sstring` lacks some of `std::string`'s methods, such as
>   `push_back()`, and its `begin()` returns a plain `char*`, not an
>   iterator.
> - Appending to them costs differently, as exercise 2 shows.
>
> Seastar can even be built with `sstring` as just another name for
> `std::string`, by turning off its CMake option `Seastar_SSTRING`, which
> is on by default. Seastar issue <https://github.com/scylladb/seastar/issues/634>
> discusses switching to `std::string` for good.

Shoal's class `request_reader` reads requests from a client's input stream. It
holds a reference to the stream, which belongs to the coroutine that
serves the client, and a buffer of data that it has read but not yet
used:

```{.cpp include="shoal/v1/resp.hh" snippet="reader"}
```

Its method `read_more()` adds whatever the client sends next to the buffer. When
the buffer is empty, the new data simply becomes the buffer; otherwise,
both are copied into a new buffer:

```{.cpp include="shoal/v1/resp.cc" snippet="read-more"}
```

This copying is simple, but not free. A read usually ends in the middle
of a request, especially when the client pipelines its requests, so most
reads find something left in the buffer, and then their data is copied
once more. [Chapter](#ch:networking) shows how to parse a request from
the stream's buffers as they are, without this copy.

`read_line()` returns the next line, and calls `read_more()` whenever
the buffer doesn't hold a whole line yet. So that a client that never
ends its line can't make Shoal allocate an unbounded amount of memory,
Shoal gives up when the buffer passes `max_line`, one of three limits that
`resp.cc` defines as Redis does:

```{.cpp include="shoal/v1/resp.cc" snippet="limits"}
```

Here is `read_line()`:

```{.cpp include="shoal/v1/resp.cc" snippet="read-line"}
```

`read()` reads a whole request, calling `read_line()` for each of its
lines. For the counts and lengths in a request, the numbers after `*` and
`$`, it calls a small function, `parse_number()`, which rejects a number
that is malformed, negative, or over the limit it is given:

```{.cpp include="shoal/v1/resp.cc" snippet="parse-number"}
```

Here is `read()`:

```{.cpp include="shoal/v1/resp.cc" snippet="read"}
```

When a request breaks the protocol, `read()` and the functions it calls
fail with `coroutine::return_exception()`. Speed hardly matters here,
since a client that breaks the protocol loses its connection anyway, but
reporting errors without throwing is a good habit in code that every
request passes through. The other two limits, on the number of
arguments and on their sizes, protect Shoal from a client that announces
a string of a petabyte.

`read()` reads each string of the request with the reader's method
`read_exactly()`. It creates the string uninitialized, copies into it
what the buffer already holds, and reads the rest directly from the
stream, with the stream's own `read_exactly()`. So a long string isn't
copied through the buffer, as data that `read_more()` reads is:

```{.cpp include="shoal/v1/resp.cc" snippet="read-exactly"}
```

### Executing commands

Shoal stores its data in a `store`, its name for a `std::unordered_map`
from keys to values, and executes requests with an ordinary function,
`execute()`:

```{.cpp include="shoal/v1/commands.hh" snippet="store"}
```

A table maps each command's name to the number of words its requests
must have, counting the command's name, which Redis calls its
[*arity*]{.index}, and to the function that executes the command:

```{.cpp include="shoal/v1/commands.cc" snippet="table"}
```

`execute()` looks the command up, checks the number of arguments, and
calls it. Redis command names are case-insensitive, so it converts the
name to lowercase first. For a wrong number of arguments, it replies with
the error that a small helper, `wrong_arguments()`, formats:

```{.cpp include="shoal/v1/commands.cc" snippet="wrong-arguments"}
```

```{.cpp include="shoal/v1/commands.cc" snippet="execute"}
```

The error is worded exactly as Redis's, so clients and tools that look
at it see what they expect. (For an unknown command, Redis also quotes the
beginning of its arguments, as in "unknown command 'FOO', with args
beginning with: 'bar'"; Shoal names only the command.)

Each command is a small function that takes the store and the request,
and returns the reply. Since `execute()` has already checked the number
of arguments, only `ping()`, whose argument is optional, checks for
itself:

```{.cpp include="shoal/v1/commands.cc" snippet="commands"}
```

### Serving clients

The coroutine that serves a client, `handle_connection()`, puts these
pieces together:

```{.cpp include="shoal/v1/main.cc" snippet="handle-connection"}
```

It reads requests until the client closes the connection, executes each
one, writes the reply, and flushes it. The flush is necessary: `write()`
only copies the reply into the stream's buffer, 8 KB by default for a
network connection, and the stream sends the buffer only when it fills,
or when the program calls `flush()` or `close()`. Without the flush, a
reply of a few bytes would stay in the buffer, while the client waits for
it, and Shoal waits for the client's next request, which never comes. A
server must therefore flush whatever it has written before it waits for
more input.

But flushing after every reply looks wasteful. Redis clients may [*pipeline*]{index="pipelining"} their requests: send many of them
at once, and only then read the replies. Sending each reply in a network
packet of its own, with a system call of its own, would be slow. But on a
network connection, Seastar's [`flush()`]{index="`seastar::output_stream`!`flush()`" .main} doesn't send anything right away.
It marks the stream as needing a flush, and returns at once; the reactor
sends the data of all the marked streams together, after running the
tasks that are ready. So when a client sends a batch of requests, Shoal
executes them one after the other, with each reply waiting in the
stream's buffer, and they all leave together. Exercise 3 shows how
effective this is.

While a client is quiet, its `handle_connection()` waits for
`reader.read()`, which waits for `read_line()`, which waits for
`read_more()`, which waits for the network: four suspended coroutines,
each with its own frame, and none of them on the thread's stack, so the
shard is free to serve other clients ([Figure](#fig:coroutine-frames)).
When the data arrives, they resume in turn.

![Shoal's request reader, waiting for a client's next request. Each
coroutine's frame, on the heap, holds its parameters and local variables
and the `co_await` where it waits, and the futures link each frame to the
one it waits for. The thread's stack holds none of them, so the reactor is
free to run other clients' coroutines. Written as ordinary functions
(right), the same calls would sit on the stack of a thread that waits for
the data too.](figures/coroutine-frames.svg){#fig:coroutine-frames}

The first line of `handle_connection()` is just as important.
[`set_nodelay(true)`]{index="`seastar::connected_socket`!`set_nodelay()`"} turns off [*Nagle's algorithm*]{.index .main}, a feature of TCP that
holds back a small packet while an earlier packet on the same connection
hasn't been acknowledged yet, hoping to send it together with more data.
On its own, that costs little, since an acknowledgment normally comes
back within a round trip. But the client's TCP has a similar feature,
[*delayed acknowledgment*]{.index .main}: it holds back the acknowledgment of the data it
receives, by 40 milliseconds or more on Linux, hoping to send it together
with data of its own. The two can end up waiting for each other. Suppose
a pipelined batch of requests reaches Shoal in two pieces:

1. Shoal executes the requests of the first piece, and sends their
   replies.
2. The client's TCP receives the replies, and delays its acknowledgment.
3. Shoal executes the second piece. Its replies make a small packet, and
   Nagle's algorithm holds it back, because the first replies haven't
   been acknowledged.
4. The client sends nothing more, because it's waiting for the rest of
   its replies, so its acknowledgment waits out the whole delay of 40
   milliseconds.

Only then does Shoal's second packet leave. Without `set_nodelay(true)`, a test program that sent Shoal pipelines of
a thousand `APPEND` requests got the replies to each pipeline about 47
milliseconds late, which made it fifteen times slower. Redis turns
off Nagle's algorithm on every connection, and a Seastar server
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

Version 1's `main()` is version 0's, with one addition: like the servers
of [chapter](#ch:tutorial-introduction), it sets
`auto_handle_sigint_sigterm` to `false`, so that Ctrl-C and `kill` end
Shoal immediately, since Shoal can't yet stop cleanly
([chapter](#ch:clean-shutdown)):

```{.cpp include="shoal/v1/main.cc" snippet="main"}
```

The store is a local variable of `shoal_main()`, and the coroutines that
serve the clients use it through a reference. That works here, because
`shoal_main()` never returns, so the store lives as long as the program.
This is the other safe case of the C++ corner on reference parameters,
earlier in this chapter: the object clearly outlives the coroutines. In
general, a background coroutine that keeps a reference to an object
must not outlive it, a subject we return to in
[chapter](#ch:object-lifetime).

## Trying it out

Build version 1 as you built version 0, and run it, here on two shards
and on port 6390, with Shoal's debug messages:

```
$ ./build/shoal -c2 -m1G --port 6390 --logger-log-level shoal=debug
INFO  [shard 0:main] shoal - Shoal listening on port 6390
```

In another terminal, use [`valkey-cli`]{index="`valkey-cli`"}, Valkey's command-line client:

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
DEBUG [shard 0:main] shoal - client 127.0.0.1:54420 connected
```

Run `valkey-cli -p 6390` without a command, and it lets you type commands
interactively.

### Tests {index="testing!Shoal's tests"}

Shoal comes with a test suite, written in Python with [`pytest`]{index="`pytest`"} and the
`redis` client library (on Fedora, the packages `python3-pytest` and
`python3-redis`). It checks each command, including values with
arbitrary bytes and values of several megabytes, and also pipelining,
inline requests, protocol errors, and many clients at once. The tests of
all the versions of Shoal are in `shoal/test/`, and the script `run`,
next to them, starts a Shoal on a free port, runs a version's tests
against it, and stops it. Its argument is the version: `./run 1` tests
the Shoal built in `shoal/v1/build`, and `--shoal` gives a different one:

```
$ cd shoal/test
$ ./run 1 -q
............... [100%]
15 passed in 0.25s
```

(`-q`, and any other options that `run` doesn't know, are passed to
`pytest`. For example, `./run 1 -v -k pipeline` runs, verbosely, only the
tests whose names contain "pipeline".)

The tests check what Redis does, not what Shoal happens to do, so they
can test any Redis-compatible server. Running them against a real one
checks the tests themselves: a test that fails against Valkey tests the
wrong thing. `./run 1 --valkey` starts a `valkey-server` instead of
Shoal:

```
$ ./run 1 --valkey -q
............... [100%]
15 passed in 0.22s
```

Writing the tests this way caught a mistake in one of them, before it
could mislead us about Shoal: it failed against both servers, because
the `redis` library interprets the reply to `PING` in a way the test
hadn't expected. [Chapter](#ch:testing) says more about testing Shoal.

### How fast is it? {index="benchmarking"}

As in [chapter](#ch:tutorial-introduction), we measured Shoal with
`valkey-benchmark`, on the same laptop with an Intel Core Ultra 7 258V.
A benchmark measures the server only if the client can send more
requests than the server can answer, so this time we gave the client
every advantage. The server runs on one fast core, and `taskset` pins
it there, with all its threads, so the operating system can't move it to
a slower core; Shoal does that itself, with `--cpuset`. The client runs
on the other three fast cores, with two threads, 96 connections, and
pipelines of 128 requests, which makes each request cheaper for the
client to send, and enough requests for each test to last half a minute
([chapter](#ch:tutorial-introduction) explained why):

```
$ ./build/shoal --cpuset 0 -m1G --port 6390
$ taskset -c 1-3 valkey-benchmark -p 6390 -t set,get -n 150000000 -P 128 -c 96 --threads 2 -q
```

and then the same with `valkey-server`, started with
`taskset -c 0 valkey-server --port 6391 --save ''`. The best of three
runs:

| Server, on one core | `SET` per second | `GET` per second |
|---|---|---|
| `valkey-server` | 2,930,000 | 3,990,000 |
| Shoal version 1 | 3,650,000 | 4,330,000 |

This time the servers, not the client, were the limit: with the same
client, Valkey answered 5.4 million `GET`s per second when the keys
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

5. In `errors.cc`, the coroutines that succeed cost about 10
   nanoseconds per call, as the table above shows. Write a version of `parse_returning()` that isn't a coroutine:
   an ordinary function that returns `make_ready_future<int>(n)` on
   success, and `make_exception_future<int>(parse_error(...))` on failure
   ([chapter](#ch:futures-and-promises) explains these functions).
   How much faster is it, on success and on failure? What does that say
   about coroutine frames?

6. Send a million `APPEND` requests in a *single* pipeline, to
   `valkey-server` and then to Shoal. What happens? While it happens,
   look at the connection with `ss -tn`: how much data waits in each
   direction? Why does Valkey finish, and Shoal not? (Hint: what does
   Shoal do while `out.write()` waits, and what does the client do?)
