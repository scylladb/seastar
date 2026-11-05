# Object lifetime in asynchronous code {#sol:object-lifetime}

## Exercise 1

Here is `sharing.cc` without the line `auto self = shared_from_this();`:

```{.cpp include="examples/solutions/object-lifetime-ex1.cc"}
```

Built normally, it printed:

```
s1: still here
s1: still here
s1: still here
s1: destroyed
owner let go; the session is gone
s1: keepalive done
later, the session is gone
```

and exited with status 0. The session is now destroyed as soon as the
owner sets its pointer to `nullptr`, since that was the only pointer to
it: `s1: destroyed` comes first, and the weak pointer is already null when
`use_session()` tests it. `keepalive()` is still asleep then, and its
frame holds only `this`, a plain pointer to the destroyed session. When
it wakes up, five milliseconds later, it reads `_closed`, and then
`_name`, from freed memory, and prints `s1: keepalive done`, a line about
a session that no longer exists. That it printed the right name, and
found `_closed` true, is luck: nothing had reused the memory yet.

Built against a `debug`-mode Seastar, the program stopped at that
moment, with exit status 1:

```
==175792==ERROR: AddressSanitizer: heap-use-after-free on address ...
    #0 ... in keepalive .../object-lifetime-ex1.cc:26
...
freed by thread T0 here:
    ...
    #6 ... in use_session .../object-lifetime-ex1.cc:40
...
previously allocated by thread T0 here:
    ...
    #3 ... in use_session .../object-lifetime-ex1.cc:35
```

Line 26 is `while (!_closed) {`, the first use of the session after
`keepalive()` woke up; the session was freed by line 40, `s = nullptr;`,
and allocated by line 35, `make_lw_shared<session>("s1")`. So
AddressSanitizer names the three places that tell the whole story: who
used the object, who destroyed it, and who made it. (The line numbers
appear only when the program is compiled with `-g`, so that the report
can name the lines; without it, the report names only the functions.)

## Exercise 2

A lambda coroutine with `[&app]`, which reads an option after sleeping:

```{.cpp include="examples/solutions/object-lifetime-ex2.cc"}
```

Built normally, it crashed, with a segmentation fault in Boost's
`program_options` library, and exit status 139. Built against a
`debug`-mode Seastar, it stopped with a "heap-use-after-free", at line
16, which reads `app.configuration()`, in memory freed by
`app_template::run()`'s code.

`app` itself is fine: it's a local variable of `main()`, and lives until
the program ends. What was freed is the lambda object, which holds the
capture, and a reference capture is stored in the lambda object, as a
pointer to `app`. As the chapter's table showed, `app_template::run()`
frees the function it was given as soon as the coroutine first
suspends, at the `co_await sleep(10ms)`. After that, the coroutine reads
the pointer to `app` from the freed lambda object, and in a `release`
build, whatever was written there since was used as `app`'s address, so
`configuration()` ran on garbage. A reference capture is safe only if
both the object it refers to *and the lambda that holds it* outlive the
coroutine; capturing by reference avoids copying the object, but not the
Fiasco.

With `this auto`, the one change:

```{.cpp include="examples/solutions/object-lifetime-ex2-fixed.cc"}
```

the program printed `smp count: 1` in both builds, and AddressSanitizer
was silent. The lambda object, with its pointer to `app`, is now moved
into the coroutine's frame when it's called, and lives as long as the
coroutine.

## Exercise 3

Here is `ptr-cost.cc`, with `use()` an ordinary function:

```{.cpp include="examples/solutions/object-lifetime-ex3.cc"}
```

The best of five runs, on one shard, on CPU 0, against the chapter's
table, which `ptr-cost.cc` measured with GCC:

| Pointer | With `noinline` and `asm` (GCC) | Without (GCC) | Without (Clang) |
|---|---|---|---|
| `int*` | 0.66 ns | 0.36 ns | 0.64 ns |
| `seastar::lw_shared_ptr<int>` | 1.18 ns | 0.44 ns | 0.47 ns |
| `seastar::shared_ptr<int>` | 1.17 ns | 0.49 ns | 0.45 ns |
| `std::shared_ptr<int>` | 13.57 ns | 13.64 ns | 14.76 ns |

Without them, Seastar's pointers cost as little as the plain pointer:
the differences among the first three rows, with each compiler, are in
the code of the loop, not in the pointers, and are a few tenths of a
nanosecond either way. `std::shared_ptr` costs what it did.

The compiler inlined `use()`, and then saw what a call does with an
`lw_shared_ptr`: increment the count, then decrement it, with nothing in
between that reads it. Those are ordinary instructions on ordinary
memory, so the compiler may combine them, and the two cancel out; the
disassembly of GCC's loop for both Seastar pointers has no instruction
that touches the count at all. `std::shared_ptr`'s loop still has its
atomic instruction, `lock xadd`, and its call to the function that
releases a reference. The rules of C++ would allow a compiler to merge
some atomic operations too, but GCC and Clang don't: an atomic operation
exists to be seen by other threads, and the compilers treat it as
something to do, not as arithmetic to simplify.

So the measurement without `noinline` and `asm` measures nothing but
the loop, for Seastar's pointers: a benchmark of something this small
has to stop the compiler from seeing that the work is useless, or it
measures the compiler's optimizer instead. But the result is also a
real advantage of Seastar's pointers, in real code: wherever the
compiler can see a copy and its destruction together, in an inlined
function that takes a pointer by value, for example, the count isn't
touched at all. With `std::shared_ptr`, the atomic operations stay,
whatever the compiler can see.

## Exercise 4

`make_lambda()` returns a lambda coroutine that captures a string of
100 characters, which is too long to be kept inside the `std::string`
object itself, so that it's in memory of its own, on the heap:

```{.cpp include="examples/solutions/object-lifetime-ex4.cc"}
```

With `--variant statement`, it printed `statement: the string has 100
characters`, in both builds, and AddressSanitizer was silent. With
`--variant later`, a `release` build printed `later: the string has
140570033721368 characters`, and a `debug` build stopped:

```
==175856==ERROR: AddressSanitizer: stack-use-after-return on address ...
    #1 ... in operator() .../object-lifetime-ex4.cc:21
...
Address ... is located in stack of thread T0 at offset 136 in frame
    #0 ... in awaited_later .../object-lifetime-ex4.cc:29
```

Line 21 is `co_return s.size();`, after the lambda's coroutine woke up,
and the memory it read was in `awaited_later()`'s stack frame.

`futurize_invoke()` takes the function by reference, calls it, and
turns what it returns into a future, which here is a future already; it
doesn't keep the function. So the lambda object is the temporary that
`make_lambda()` returned, in the statement that calls
`futurize_invoke()`, and a temporary lives until the end of that
statement. In `in_one_statement()`, the statement ends only after the
`co_await` has waited for the lambda's coroutine, so the temporary
outlives it. In `awaited_later()`, the statement `auto f =
futurize_invoke(make_lambda());` ends at once, and the temporary, with
`s`, is destroyed while the coroutine sleeps. (The compiler kept the
temporary on the stack, rather than in `awaited_later()`'s frame, since
it doesn't live across a `co_await`, which is why AddressSanitizer
reports a stack-use-after-return, not a heap-use-after-free.)

So the result depends on how `futurize_invoke()` is called, not on the
function: it behaves exactly like calling the lambda directly, the
table's two rows "a call awaited at once" and "a call stored and awaited
later". That's the general rule for any function that calls its
argument without keeping it: it's as safe as the statement around it.
And `this auto` makes both variants safe, since the lambda is then moved
into the frame before the statement ends.
