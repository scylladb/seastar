# Object lifetime in asynchronous code {#ch:object-lifetime}

In synchronous code, an object's lifetime takes care of itself. A local
variable lives until its function returns, a function's caller waits for
it, and an object passed by reference outlives the call because the
caller's frame does. In asynchronous code, none of this holds. A
coroutine that waits gives the CPU back to its caller, which may go on,
return, and destroy its own objects, while the coroutine still means to
use them when it wakes up. A background operation has no caller waiting
for it at all. And an object that one fiber uses may be destroyed by
another while the first one waits. The result is code that reads freed
memory: it may print garbage, crash, or seem to work, until it doesn't.

Of all the bugs in Seastar programs, this is the most common, and the
hardest to spot by reading the code, because the mistake is in what the
code *doesn't* say: who keeps an object alive while it's in use. This
chapter is about saying it. It shows what a coroutine keeps alive by
itself and what it doesn't, the conventions that Seastar's functions
follow, the tools for giving ownership away or sharing it, the traps of
`this` and of lambda coroutines, and how to catch the mistakes that slip
through, with a bug that we plant in Shoal on purpose.

## What a coroutine keeps, and what it doesn't

As [chapter](#ch:coroutines) explained, a coroutine's frame holds copies
of its parameters, and its local variables, for as long as the coroutine
runs, across all its `co_await`s. So everything a coroutine holds *by
value* is safe. The trouble is everything else: anything the coroutine
reaches through a reference, a pointer, or an object that refers to
memory it doesn't own. The frame holds only the reference; the object
it refers to lives wherever it lives, and nothing ties its lifetime to
the coroutine's.

Some of these are obvious, and some are references in disguise:

- References and pointers, of course, including a parameter declared
  `const T&`, which [chapter](#ch:coroutines)'s C++ corner showed can
  dangle.
- [Views]{index="views (string_view, span)"}, such as `std::string_view` and `std::span`, which point into
  someone else's characters or elements.
- [Iterators]{index="iterator invalidation"}, which point into a container, and stop being valid when the
  container changes, as `KEYS` learned in
  [chapter](#ch:how-seastar-runs-your-code).
- `this`, in a method that is a coroutine: its frame holds the
  pointer, not the object.
- A lambda's captures, in a lambda that is a coroutine: they live in the
  lambda object, not in the frame, as we will see.

For each of these, a coroutine that uses it across a `co_await` raises
two questions. Will the object still exist when the coroutine wakes up?
And if it does, may another fiber have changed it in the meantime?
We met the second question in [chapter](#ch:how-seastar-runs-your-code),
where other clients changed the store while `KEYS` yielded, and in
[chapter](#ch:concurrency-within-a-shard), where two fibers wrote to one
connection. This chapter is about the first.

## Passing objects to asynchronous functions

### The caller keeps it alive

Seastar's own asynchronous functions often take arguments by reference,
or by pointer, and they all follow one convention: [the caller must keep
the object alive until the returned future is
available]{index="caller keeps arguments alive"}. A file's `dma_write()`, for example, takes a pointer to the data
to write, and its documentation says that the buffer must exist until
the future is ready. The function starts the write and returns at once,
while the disk still reads from the buffer. The same goes for an
`input_stream<char>&` given to a function such as
[chapter](#ch:coroutines)'s `count_bytes()`, or a `semaphore&` given to
functions that we'll meet in [chapter](#ch:coordination).

The convention is natural when the caller awaits the call right away, as
it usually does:

``` cpp
co_await f.dma_write(pos, buf.get(), size);
```

Here the caller's frame, which owns `buf`, waits for the write, so `buf`
outlives it. The convention becomes a trap only when the caller doesn't
wait: when it keeps the future to wait for later, after its objects are
gone, or drops the future, and lets the operation run in the background.

Your own coroutines are such functions too, and the same convention
applies to their reference parameters. But your callers won't read a
documentation comment, so [chapter](#ch:coroutines) gave a simpler rule
for them: take parameters by value, unless every caller awaits the call
in the same statement, or the object clearly outlives the coroutine.

### Giving ownership away

A background operation has no caller waiting for it, so it must own
what it uses, or use what is guaranteed to outlive it.
[Chapter](#ch:concurrency-within-a-shard)'s `FLUSHALL ASYNC` showed the
simplest way: a coroutine that takes its data *by value*, and a caller
that moves the data into it, with `std::move()`. The coroutine's frame
then owns the data, until the coroutine ends.

Moving works for anything movable, and for objects that can't be moved
cheaply, or at all, a `std::unique_ptr` can be moved instead: the object
stays where it is, on the heap, and the pointer, with the ownership,
moves into the frame. Seastar's own types follow the same idea.
[`temporary_buffer`]{index="`seastar::temporary_buffer`!ownership"}, from [chapter](#ch:coroutines), owns its memory, can be
moved but not implicitly copied, and so has exactly one owner at a time,
which code can pass along with `std::move()`, without copying a byte.

Code written before coroutines had a harder problem, because a
continuation has no frame in which to keep its objects. It used
[`do_with()`]{index="`seastar::do_with()`"}, from `<seastar/core/do_with.hh>`, which keeps objects alive until the future returned by a
function given to it is available. You will see it in older code, and
[chapter](#ch:continuations) explains it; in a coroutine, a local
variable does the same.

Moving an object into a call has one trap of its own, which has nothing
to do with Seastar.

> **C++ corner: the order of a function's arguments.**[]{index="C++ corner!the order of a function's arguments"}[]{index="evaluation order"}
> C++ doesn't say in which order a function's arguments are evaluated.
> So a call that moves from an object, and also uses it, in two
> arguments, may see the object before the move, or after it:
>
> ```{.cpp include="examples/object-lifetime/order.cc" snippet="order"}
> ```
>
> Built with GCC, this program prints `got 100 bytes, told 100`; built
> with Clang, `got 100 bytes, told 0`, because Clang moved `s` into the
> first parameter before it evaluated `s.size()`. Both are correct C++.
> The fix is to read what you need into a variable of its own before the
> call. (C++17 did fix one similar case, which older Seastar code relies
> on: in `p->get().then([p = std::move(p)] { ... })`, everything to the
> left of `.then` is evaluated before its arguments, so `p` is used
> before it's moved.)

## Sharing ownership

Sometimes an object has no single owner. Several fibers use it, none of
them knows which will finish last, and the object should live until the
last one is done with it. [Chapter](#ch:concurrency-within-a-shard)'s
Pub/Sub had this problem, with clients that publishers write to, and
solved it by making the owner wait: a client's connection doesn't
destroy the client until it has removed it from all its channels, and
waited for the writes already started. The other way is to let the users
share the ownership, by counting them.

### `lw_shared_ptr` and `shared_ptr`

A [*reference-counted*]{index="reference counting" .main} pointer, such as C++'s own `std::shared_ptr`, keeps a
count, next to the object, of the pointers that point to it. Copying a
pointer increments the count, destroying one decrements it, and when the
count reaches zero, the last pointer destroys the object. However,
Seastar applications shouldn't use `std::shared_ptr`: Seastar has better
alternatives, two of them, in fact.

Seastar's shared pointers are faster than `std::shared_ptr`, because
`std::shared_ptr` must work on many cores: pointers to the same object
may be copied and destroyed on different threads at the same time, so it
updates its count with atomic operations. A Seastar
object belongs to one shard, so Seastar's pointers update their counts
with ordinary instructions. To measure the difference, we pass such a
pointer, by value, to a function that does nothing with it: the copy
increments the count, and destroying it decrements it. Here is the time
each call took (the best of ten runs, on one shard):

| Pointer | Time per call |
|---|---|
| `int*` | 0.66 ns |
| `seastar::lw_shared_ptr<int>` | 1.18 ns |
| `seastar::shared_ptr<int>` | 1.17 ns |
| `std::shared_ptr<int>` | 13.57 ns |

The plain pointer has no count, and shows what the call itself costs.
But it isn't safe, of course: it says nothing about who keeps the object
alive, which is the subject of this chapter. Seastar's pointers add
about half a nanosecond, for an ordinary increment and decrement.
`std::shared_ptr` adds about thirteen, for its atomic operations,
even though only one core ever touches its count.

Seastar has two such pointers, both from `<seastar/core/shared_ptr.hh>`.
The first, [`shared_ptr<T>`]{index="`seastar::shared_ptr`" .main}, is
the direct replacement for `std::shared_ptr<T>`. It's made with
`seastar::make_shared<T>()`, it converts from a pointer to a derived
class to a pointer to its base class, and it has `static_pointer_cast()`
and `dynamic_pointer_cast()`. Its one important difference is the one
above: its count isn't atomic, so it can't be used on more than one
shard. It takes the same memory as `std::shared_ptr`: the pointer is two
machine words, one pointing to the object and one to the count, and the
object carries two more words, the count and the pointer to a virtual
table that knows how to destroy the object. (`std::make_shared()`, in
GCC's standard library, adds the same two words, with two counts of four
bytes each, for strong and weak pointers.) And like `std::shared_ptr`, it
can point to a class that has only been declared, not defined, as long
as `make_shared()` sees the definition. What it lacks are
`std::shared_ptr`'s less common features: it can only be made with
`make_shared()`, never from a pointer to an object allocated with `new`,
so it has no custom deleters; and it has no weak pointers (Seastar's
weak pointers, later in this chapter, work differently).

The second, [`lw_shared_ptr<T>`]{index="`seastar::lw_shared_ptr`" .main}
("lightweight"), is cheaper still, when you don't need polymorphism:
converting a pointer to a derived class into a pointer to its base
class, say, a pointer to a `circle` into a pointer to a `shape`, and
using the object through it. A `shared_ptr<shape>` that turns out to be
the last pointer to a `circle` must destroy a `circle`, not a `shape`.
That's what the virtual table next to its count is for, and why the
pointer is two words: one points to the object, as a `shape`, and the
other to the count, which knows the object's real type. An
`lw_shared_ptr<T>` always points to a whole `T`, which it knows how to
destroy, so it needs neither: the pointer is one word, and the object
carries one more word, its count. An object of 16 bytes, for example,
takes 24 bytes instead of 32, and each pointer to it, kept in other
objects, half the space. It's made with
[`make_lw_shared<T>()`]{index="`seastar::make_lw_shared()`"}.

Another limitation of `lw_shared_ptr` is that `T` must be defined
wherever an `lw_shared_ptr<T>` may be destroyed, so a class can't simply
hold an `lw_shared_ptr` to a class that is only declared. It can, with
a little
more work: next to the declaration of `T`, specialize the template
[`lw_shared_ptr_deleter<T>`]{index="`seastar::lw_shared_ptr_deleter`" .main}
with a static method `dispose(T*)`, which destroys the object,
usually with `delete`; derive `T` from `enable_lw_shared_from_this<T>`,
described below; and include `<seastar/core/shared_ptr_incomplete.hh>`
after the definition of `T`, in the file that defines `dispose()`.
ScyllaDB and Redpanda both do this for classes that many files point
to, and `dispose()` has another use, which we'll see later in this
chapter.

Use `lw_shared_ptr` unless you need the polymorphism. ScyllaDB and
Redpanda together write `lw_shared_ptr` about 2,500 times, and Seastar's
`shared_ptr` about 2,000 times, many of them for class hierarchies where
the polymorphism is needed, such as ScyllaDB's CQL functions and
statements. `std::shared_ptr` appears fewer than a hundred times, mostly
where an object really is shared by several shards, such as a barrier
that all the shards wait on, or held in an exception, which may travel
to another shard. Redpanda explains one such use in a comment: "the use
of `std::shared_ptr<>` is generally discouraged in the redpanda codebase
because of the overhead of the atomic reference count", but here "we
need a way to keep track of a reference count across multiple shards,
which is exactly what `std::shared_ptr<>` is meant for."

Remember that Seastar's shared pointers, both of them, must never be
shared between shards: they update their counts with ordinary
instructions, so two shards that copy pointers to the same object at the
same time would corrupt its count. [Chapter](#ch:shared-nothing) shows
`foreign_ptr`, the way to pass ownership to another shard.

An object can also give out pointers to itself, from a method,
if its class derives from
[`enable_lw_shared_from_this<T>`]{index="`seastar::enable_lw_shared_from_this`" .main}
(or [`enable_shared_from_this<T>`]{index="`seastar::enable_shared_from_this`" .main},
for `shared_ptr`): its `shared_from_this()` returns an `lw_shared_ptr`
(or a `shared_ptr`) that shares the count of the pointers that already
point to it. This is how a
member coroutine that runs in the background can keep its own object
alive:

```{.cpp include="examples/object-lifetime/sharing.cc" snippet="session"}
```

`keepalive()` starts by taking a pointer to its own session, which lives
in its frame, so the session can't be destroyed while `keepalive()` runs,
whoever else lets go of it. (The session also derives from
`weakly_referencable<session>`, only so that the program can watch it
with a weak pointer, and print when it's gone; weak pointers are
explained below.) Its owner can start it in the background,
and forget about it:

```{.cpp include="examples/object-lifetime/sharing.cc" snippet="use"}
```

The program prints:

```
s1: still here
s1: still here
s1: still here
owner let go; the session still exists
s1: keepalive done
s1: destroyed
later, the session is gone
```

When the owner sets its pointer to `nullptr`, the session survives,
because `keepalive()` still holds a pointer to it; it's destroyed only
when `keepalive()` ends, after it notices that the session was closed
([Figure](#fig:shared-session)).

![The life of `sharing.cc`'s session. The owner and `keepalive()` each
hold a pointer to it. The owner closes it and lets go at 25 ms, but the
session lives on, with one pointer, until `keepalive()` wakes up, sees
that it was closed, and ends, at 30 ms; the owner's weak pointer later
finds it gone. A thick line shows a pointer
held.](figures/shared-session.svg){#fig:shared-session}

Without the first line of `keepalive()`, the session would be destroyed
while `keepalive()` still uses it, as exercise 1 shows.

Reference counting is convenient, but it has costs beyond the counting.
It hides who owns an object, so that it can be hard to know when, or
whether, the object will be destroyed; and two objects that hold
pointers to each other keep each other alive forever. It suits objects
with no natural owner, and a natural end, like a session that ends when
its last user is done. For objects with an owner, such as a server's
services, the owner waiting for the users, as in
[chapter](#ch:concurrency-within-a-shard), is usually clearer.

Seastar counts references inside some of its own types, too. A
`temporary_buffer`'s [`share()`]{index="`seastar::temporary_buffer`!`share()`"}, which
[chapter](#ch:coroutines) mentioned, makes another buffer for part of
the same memory, without copying it, and a reference count keeps the
memory alive until the last of the buffers that share it is destroyed.
So a buffer made by `share()` is as safe to keep across a `co_await` as
a shared pointer. It has the cost of reference counting, too, in a form
that is easy to miss: a small piece of a large buffer, kept with
`share()`, keeps the whole large buffer alive, which is why Seastar's
documentation warns against holding a `temporary_buffer` for long.

### Weak pointers

The program above also used a pointer of a third kind. A
[`weak_ptr<T>`]{index="`seastar::weak_ptr`" .main}, from `<seastar/core/weak_ptr.hh>`, refers to an object without keeping it alive: when the
object is destroyed, every `weak_ptr` to it becomes null, and testing it
tells whether the object still exists. A class that allows weak pointers
derives from
[`weakly_referencable<T>`]{index="`seastar::weakly_referencable`"}, and gives them out with
`weak_from_this()`. Unlike `std::weak_ptr`, it doesn't need the object to
be reference-counted at all: the object can have a single owner, of any
kind, and the weak pointers just watch it.

A weak pointer suits code that wants to use an object if it's still
there, without being the reason it's there: a cache of connections, say,
or a background task that should simply stop when the object it works
for goes away. Each time such code wakes up from a `co_await`, it checks
its weak pointer before it uses the object.

(Seastar also has [`checked_ptr`]{index="`seastar::checked_ptr`"}, from
`<seastar/core/checked_ptr.hh>`, a wrapper for a pointer that throws an
exception when a null pointer is used, instead of crashing. It's rarely
used, and doesn't help with objects that were destroyed: a pointer to
freed memory isn't null.)

## `this` in member coroutines

A method that is a coroutine keeps `this`, a pointer, in its
frame, so the object must outlive the coroutine, which is easy when the caller awaits
it, and the object belongs to the caller. It's a trap when the coroutine
runs in the background, because nothing then stops the object's owner
from destroying it:

``` cpp
(void)conn->keepalive();    // runs in the background...
conn = nullptr;             // ...while its object is destroyed
```

There are two ways to make this safe. One is for the coroutine to share
the ownership, as `session::keepalive()` does, with a pointer from
`shared_from_this()` in its frame. The other is for the owner to wait,
before it destroys the object, for the background work that uses it to
finish: a `gate`, which [chapter](#ch:coordination) shows, counts such
work, and lets the owner wait for all of it. Whichever you choose, a
background member coroutine without either is a bug, whether or not
anyone has noticed yet.

## Lambda coroutines

A lambda can be a coroutine too, and lambdas are where Seastar programs
most often go wrong, because a lambda's captures look like local
variables, but aren't.

> **C++ corner: lambdas are objects.**[]{index="C++ corner!lambdas are objects"}[]{index="lambda!captures"}
> A lambda expression, which C++11 added, creates an object, of a class
> that the compiler generates. Each capture is a data member of that object: `[name]`
> copies `name` into a member, `[&name]` stores a reference in one,
> `[n = std::move(name)]` move-constructs a member named `n` (a capture
> with an initializer, which C++14 added), and
> `[this]` stores the pointer. The lambda's body becomes the class's
> `operator()`, a method, which finds the captures through its
> own `this`, a pointer to the lambda object. Captured members are
> `const` unless the lambda is declared `mutable`. So
> `[name] () -> future<> { ... }` is roughly:
>
> ``` cpp
> struct lambda {
>     std::string name;
>     future<> operator()() const { ... }
> };
> ```

### The Lambda Coroutine Fiasco {index="Lambda Coroutine Fiasco"}

When the lambda's `operator()` is a coroutine, its frame holds what any
member coroutine's frame holds: a pointer to its object. The captures
stay in the lambda object, and the coroutine reaches them through that
pointer. So the coroutine can use its captures only as long as the lambda
object exists, and the lambda object belongs to whoever called the
lambda, not to the coroutine.

![The Lambda Coroutine Fiasco, in `greet_later()`, with time running to
the right. Top: `then()`'s task holds the lambda object, with its
capture `name`, and frees it when the call returns, the first time the
coroutine waits; the coroutine's frame still points to the lambda, and
reads freed memory when it resumes. Bottom: with `this auto`, the lambda
is moved into the frame when it's called, so its capture lives as long
as the coroutine.](figures/fiasco.svg){#fig:fiasco placement="tb"}

That is fine when the caller keeps the lambda object until the
coroutine ends. It's a bug, known in Seastar as the
[*Lambda Coroutine Fiasco*]{index="Lambda Coroutine Fiasco" .main}, when the caller destroys the lambda
object as soon as the call returns, which, for a coroutine, means as
soon as it first suspends. The classic example is `then()`, a method of
every future, which calls a function, often a lambda, with
the future's value once it's available. Seastar code written before
coroutines is built from it, and [chapter](#ch:continuations) explains
it:

```{.cpp include="examples/object-lifetime/fiasco.cc" snippet="wrong"}
```

`then()` moves the lambda into the task that will call it, and that task
is freed right after the call, once the lambda's coroutine has returned
its future: the first time the coroutine waits
([Figure](#fig:fiasco)). When the coroutine wakes up, 10 milliseconds
later, `name` is in freed memory. When we ran it, it printed
`hello, ` followed by a few bytes of garbage and the end of the name,
and exited normally; built with AddressSanitizer, which we meet at the
end of this chapter, it reported a "heap-use-after-free", in memory that
`seastar::continuation`, `then()`'s task, had freed.

Which other functions destroy the lambda they're given as soon as
their call to it returns, before its coroutine has finished? We gave a lambda coroutine
that captures a string, waits, and then reads its capture, to many of
Seastar's functions, in a program built with AddressSanitizer:

| Given the lambda coroutine | Its captures |
|---|---|
| `then()` | freed while it waits |
| `app_template::run()` | freed while it waits |
| a call stored and awaited later: `auto f = lambda(); co_await std::move(f);` | freed while it waits |
| a call awaited at once: `co_await lambda();` | safe |
| `smp::submit_to()`, `smp::invoke_on_all()` | safe |
| `coroutine::all`, `when_all_succeed()` | safe |
| `coroutine::parallel_for_each`, `parallel_for_each()`, `max_concurrent_for_each()` | safe |
| `with_gate()`, `do_with()` | safe |

The safe functions keep the callable until the future it returned is
available, as their documentation promises: `coroutine::all`'s says so
explicitly, which is why [chapter](#ch:concurrency-within-a-shard)
could give it lambda coroutines. `then()` can't keep it, because it was
written for continuations, which return a future and are done.
`app_template::run()` frees its function early too, which is why
[chapter](#ch:getting-started)'s Shoal passes to `run()` a lambda that
isn't a coroutine, and calls a coroutine function from it.

The table can't be complete, and a new function's behavior isn't always
obvious from its name. A lambda coroutine that captures nothing is
always safe, since it has nothing to lose. For one that captures, there
is a better answer than checking each function's documentation.

### The cure: deducing `this`

C++23 added a feature called [*deducing this*]{index="deducing this" .main}: a method, or a
lambda, can declare its object as an explicit first parameter, written
`this auto`, or `this auto&`, and so on. With `this auto`, the object is
passed *by value*. For a lambda coroutine, that is exactly the cure: when
the lambda is called, the lambda object, with its captures, is copied or
moved into the coroutine's frame, like any other parameter, and lives as
long as the coroutine does, whatever happens to the original:

```{.cpp include="examples/object-lifetime/fiasco.cc" snippet="right"}
```

This version prints `hello, ` and the whole name, and runs cleanly
with AddressSanitizer. The cost is a copy or a move of the lambda's
captures into the frame, usually negligible. Since Seastar requires
C++23, the cure is always available, and a good habit is to write
"`this auto`" in every lambda coroutine that captures anything.

> **C++ corner: where `this auto` came from.**[]{index="C++ corner!where `this auto` came from"}
> Every non-static method has a hidden parameter: the object it
> was called on, which it reaches through the pointer `this`. C++11 let a
> method say what kind of object it accepts, with `&` or `&&`
> after its parameter list, so that it can, for example, move from the
> object when it's a temporary. With `const`, that makes four versions of
> one function: `std::optional<T>::value()`, for example, has four
> overloads, for `&`, `const&`, `&&` and `const&&`, which differ only in
> their types. The proposal "Deducing this" (P0847), by Gašper Ažman, Sy
> Brand, Ben Deane and Barry Revzin, which C++23 adopted, replaces them
> with one template, by making the hidden parameter an ordinary one: the
> method's first parameter, with the keyword `this` written before its
> type, as in `this Self&& self` in this class:
>
> ``` cpp
> template <typename T>
> class holder {
>     T _value;
> public:
>     template <typename Self>
>     auto&& get(this Self&& self) {
>         return std::forward<Self>(self)._value;
>     }
> };
> ```
>
> The `&&` here doesn't mean that the object must be a temporary. When the
> type before `&&` is a template parameter that the compiler deduces,
> `Self&&` is a [*forwarding reference*]{index="forwarding reference" .main}, which binds to any object, and
> `Self` records what it was: `holder<T>&` when `get()` is called on a
> variable, `const holder<T>&` on a `const` one, and `holder<T>` or
> `const holder<T>` on a temporary. `Self&&` then becomes the matching
> reference (`&` and `&&` together make `&`), and `std::forward<Self>()`
> passes the object on as it came: a variable stays a variable, and a
> temporary may be moved from.
>
> Inside, there is no `this` pointer: the method reaches its object
> through that parameter, `self`, as Python and Rust functions do through
> theirs, which the paper compares it to. The committee debated other
> spellings over the paper's revisions before settling on marking the
> parameter with a keyword that already existed.
>
> The object can also be passed by value, which is what this chapter
> needs. The parameter's type is then the class itself, with no `&`, so
> the method names its own class, as this method of `holder` does:
>
> ``` cpp
>     T take(this holder self) {
>         return std::move(self._value);
>     }
> ```
>
> Unlike `get()`, `take()` isn't a template: it's an ordinary method,
> whose first parameter has the type `holder`, which, inside the class,
> means `holder<T>`. `self` is a copy of the object that `take()` was
> called on, or that object moved, if it was a temporary, and like any parameter passed by
> value, it lives until the call ends, or, in a coroutine, until the
> coroutine ends, whatever happens to the original.
>
> Lambdas got the feature too, since a lambda's body is a method of the
> lambda's class. But that class, which the compiler generates, has no
> name, so a lambda can't write `this holder`, or `this const holder&`.
> It writes `auto` instead, which, as a parameter's type, stands for a
> deduced template parameter, like `Self` above. So `this auto` is a
> lambda's `this holder`: the lambda object, passed by value, with its
> captures. (And `this auto&&` is its `this Self&&`.) Among its uses, the paper lists lambdas that call themselves,
> through that parameter, and, in a section titled "For lifetime
> management of coroutines", this chapter's very problem. A coroutine
> avoids dangling references by taking its parameters by value, which
> works for every parameter "except the implicit object parameter, which
> is always a reference." Now that one can be taken by value too.

Older Seastar code, written before C++23, has two workarounds that you
should recognize. One is
[`coroutine::lambda`]{index="`seastar::coroutine::lambda`"}, a wrapper that `then()` can move without moving
the lambda itself, which stays a temporary of the statement that awaits
the result:

```{.cpp include="examples/object-lifetime/fiasco.cc" snippet="old"}
```

It works only when the statement awaits the result, so that the
temporary lives long enough. The other is to give the lambda a name, so
that it lives in the enclosing coroutine's frame, and pass `std::ref()`
to it. Both still work, but `this auto` is simpler, and works in every
case.

## Constructors and destructors can't wait

One more consequence of asynchronous lifetimes is visible in the design
of Seastar's classes.

> **C++ corner: no coroutines among constructors and destructors.**[]{index="C++ corner!no coroutines among constructors and destructors"}
> A constructor or a destructor can't be a coroutine: C++ forbids
> `co_await` and `co_return` in them. So they can do only what they can
> do without waiting.

So an object whose creation needs to wait, for example to open a file,
can't do it in its constructor. Seastar code uses an
[asynchronous factory]{index="asynchronous factory function"} instead: a static method, such as
`static future<lw_shared_ptr<T>> create(...)`, that waits for what it
needs, and then constructs the object. And an object whose destruction
needs to wait, to flush buffered data or to let operations in progress
finish, has an asynchronous `close()` or `stop()` that its owner must
call, and await, before destroying it. That is why the streams of
[chapter](#ch:coroutines) have a `close()`, and why Seastar complains
when an output stream with unsent data is destroyed without one.
[Chapter](#ch:clean-shutdown) lists the objects that need this, and
the helpers that make sure it happens even when an exception interrupts
the code.

Shared ownership makes this harder, because no one owner knows that it's
the last: the last pointer to go away might be anywhere. But
`lw_shared_ptr` lets a class choose what happens then: a specialization
of `lw_shared_ptr_deleter<T>`, whose static method `dispose()` the last
`lw_shared_ptr<T>` calls, instead of deleting the object itself. This
chapter used it earlier for classes that are only declared, but it
helps here too, because `dispose()` needn't delete the object at
once. It can start closing it, in the background, and delete it when
that's done.

This program, `closing.cc`, shows how. Its `segment` must be closed,
like a file, before it's deleted, and closing it waits. The
specialization is declared before the class, as for a class that is
only declared:

```{.cpp include="examples/object-lifetime/closing.cc" snippet="deleter"}
```

`dispose()` calls the segment's `unused()`, and the last pointer to a
segment goes away twice:

```{.cpp include="examples/object-lifetime/closing.cc" snippet="segment"}
```

The first time, `unused()` starts closing the segment, and needs the
segment to stay alive meanwhile: so it makes a new pointer to it with
`shared_from_this()`, which brings the count back from zero to one, and
keeps that pointer in the continuation that runs when the closing ends.
When the continuation is destroyed, that pointer, now the last, goes
away, `dispose()` is called a second time, and `unused()`, seeing
`_closing`, deletes the segment.

The closing runs in the background, after every pointer that the
program knew about is gone, so nothing would wait for it when the
program ends: the reactor would stop before the segment was closed, and
the segment would be leaked. So the program uses a
[`gate`]{index="`seastar::gate`"}, which counts background work and lets its owner wait for all of it.
[Chapter](#ch:coordination) explains gates; this program needs only two
of their methods: `hold()` returns a holder that counts as one piece of work until it's
destroyed, and `close()` returns a future that's ready when no holder is
left. Each closing segment keeps a holder in the continuation, next to
its pointer, and the program's end waits for the gate:

```{.cpp include="examples/object-lifetime/closing.cc" snippet="main"}
```

It prints:

```
two pointers to segment 1
segment 1: unused, closing
no pointers left; shutting down
segment 1: closed
segment 1: deleted
done
```

ScyllaDB's sstables, the files that hold its data, are shared this
way: their `dispose()` hands an unused sstable to the sstables manager,
which closes its files in the background, and keeps a list of the
sstables still closing, so that ScyllaDB's shutdown can wait for them.

## Finding lifetime bugs {index="AddressSanitizer"}

Lifetime bugs are hard to find by testing, because reading freed memory
usually seems to work: the memory still holds its old contents, until
something reuses it. To see how hard such a bug is to find, and how to
find it anyway, let's plant one in a copy of Shoal version 4. Its
`FLUSHALL ASYNC`, in [chapter](#ch:concurrency-within-a-shard), moves
the data out of the store into a local variable, `old`, and then into a
coroutine that destroys it gradually in the background:

``` cpp
store old = std::exchange(db, store());
// ...
(void)destroy_gently(std::move(old));
```

`destroy_gently()` takes its `store` by value, so the data lives in the
coroutine's frame until the coroutine is done with it. We replace that
call with the one that chapter warned against:

``` cpp
(void)clear_gently(old);    // the planted bug
```

`clear_gently()` takes its container by reference, and `old` is a local
variable of `flushall()`, which lives in `flushall()`'s frame. So when
`flushall()` replies and ends, `old` is destroyed, while `clear_gently()`
is still erasing keys from it in the background.

Now we load about 330,000 keys into the buggy Shoal, built as usual,
and send `FLUSHALL ASYNC`. Shoal crashes at once, with a segmentation
fault and a backtrace of bare addresses. That's a lucky outcome, because the
crash points near the bug. Reading freed memory can just as well seem to
work, as exercise 1 shows, or corrupt memory quietly, and crash much
later, somewhere else.

Then we build the same Shoal against a Seastar built in `debug` mode, as
[appendix](#app:building-seastar) shows. Shoal's `CMakeLists.txt`
doesn't change: Seastar's CMake package tells the compiler to build Shoal the
way Seastar was built, which in `debug` mode includes
[*AddressSanitizer*]{index="AddressSanitizer" .main}, a feature of GCC and Clang that checks every
access to memory, and remembers where each block of memory was allocated
and freed. The same test now ends with this report (shortened here):

```
ERROR: AddressSanitizer: heap-use-after-free on address 0x7c4dd61e2e48
READ of size 8 at 0x7c4dd61e2e48 thread T0
    ...
    #3 in clear_gently commands.cc:152
    #4 in std::coroutine_handle<...>::resume() const
    ...
freed by thread T0 here:
    #0 in free.part.0
    #1 in seastar::internal::coroutine_allocators::operator delete(void*, unsigned long)
    #2 in flushall commands.cc:218

previously allocated by thread T0 here:
    #0 in malloc
    #1 in seastar::internal::coroutine_allocators::operator new(unsigned long)
    #2 in flushall commands.cc:202
```

It names the whole story. Line 152 is the test of `clear_gently()`'s
loop, `while (!c.empty())`, reading the table after the coroutine woke
up. The memory was allocated at line 202, the start of `flushall()`, and
freed at line 218, its end, by the operator that frees coroutine frames:
the table lived in `flushall()`'s frame, which was freed when
`flushall()` ended. A bug that crashed with a backtrace of addresses in
the ordinary build is explained in three lines in the `debug` build.

`debug` mode does more than AddressSanitizer. It replaces Seastar's
memory allocator with the system's, so that AddressSanitizer sees every
allocation; it checks that every `lw_shared_ptr` and `shared_ptr` is
used only on the shard that created it; and it runs ready tasks in a
random order, to expose code that silently depends on an order that
Seastar doesn't promise. It's much slower, too: the buggy Shoal reports
stalls of tens of milliseconds while it deletes the keys. So it's for
tests, not for production, and every Seastar project should run its
tests in `debug` mode, as ScyllaDB and Seastar's own continuous
integration do. The chapter's other lifetime bugs, the Lambda
Coroutine Fiasco in `fiasco.cc` and those in the exercises below, are
caught the same way in `debug` mode, at the first access to freed
memory.

## Exercises

1. In `sharing.cc`, delete the first line of `session::keepalive()`, the
   one that takes a pointer from `shared_from_this()`. Run the program
   built normally: what does it print, and in what order? Then build it
   against a `debug`-mode Seastar, and run it again. Which line does
   AddressSanitizer point to?

2. [Chapter](#ch:getting-started) passes `run()` a lambda that isn't a
   coroutine. Write a
   program whose lambda *is* a coroutine, but captures `app` by
   reference, `[&app]`, which seems safe, since `app` lives as long as
   `main()`: the coroutine sleeps for 10 milliseconds, and then reads an
   option from `app.configuration()`. Run it built normally, and in
   `debug` mode. What happens, and why? Then add `this auto`, and run it
   again.

3. The program that measured the table of pointer costs is
   `ptr-cost.cc`. The function that it calls, `use()`, is marked
   `[[gnu::noinline]]`, and contains an empty `asm` statement, which tells
   the compiler that the pointer is used by code that it can't see.
   Remove both, and measure the four kinds of pointer again, built with GCC
   and with Clang. Which results change, and why? What do they tell you
   about measuring small costs, and about what the compiler can do with
   ordinary instructions that it can't do with atomic ones?

4. The table of functions earlier in this chapter doesn't include
   `futurize_invoke()`, which calls a function and turns whatever it
   returns into a future. Find out, with a `debug` build, whether it keeps
   a lambda coroutine's captures alive: try both
   `co_await futurize_invoke(make_lambda())`, in one statement, and a
   future kept in a variable and awaited later, where `make_lambda()`
   returns a lambda coroutine that captures a string. Does the result
   depend on the function, or on how you call it?
