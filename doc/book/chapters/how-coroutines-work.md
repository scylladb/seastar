# How coroutines work {#ch:how-coroutines-work}

The earlier chapters used coroutines by rules. Every `co_await` is a
preemption point ([chapter](#ch:how-seastar-runs-your-code)). A
reference parameter may die while the coroutine waits, and a coroutine
lambda's captures die at its first `co_await`, which is the Lambda
Coroutine Fiasco ([chapter](#ch:object-lifetime)). A coroutine costs more
than a continuation that finds its future available
([chapter](#ch:continuations)). This chapter shows why, from the inside:
what C++20 provides, how Seastar builds its coroutines on it, which
turns out to be a few dozen lines, and how to use the same machinery to
write awaitables and generators of your own.

## What C++20 provides {index="coroutine!how it works"}

C++20 doesn't define what a coroutine *does*; it defines a protocol, and
leaves the rest to a library, here Seastar. A function is a coroutine if
its body uses `co_await`, `co_yield` or `co_return`. For a coroutine, the
compiler does three things.

It moves the function's state off the stack, into a *frame* that it
allocates: the parameters, the local variables that live across a
suspension, and where the coroutine is in its body. The function can then
return to its caller in the middle, and continue later, from the frame,
on a stack that may be another function's.

It asks the coroutine's return type what to do, through a *promise
type*, which `std::coroutine_traits<R, Args...>::promise_type` names,
and which is usually the return type's own nested `promise_type`. The
frame holds an object of this type, and the compiler turns the body,
roughly, into this:

```cpp
promise_type promise;                       // in the frame
R result = promise.get_return_object();     // what the caller gets
co_await promise.initial_suspend();
try {
    ... the body; co_return x calls promise.return_value(x) ...
} catch (...) {
    promise.unhandled_exception();
}
co_await promise.final_suspend();
// and then the frame is freed
```

And it turns each `co_await expr` into a call of three methods of an
*awaiter*, which `expr` either is or provides. `await_ready()` says
whether the result is already there; if it is, the coroutine doesn't
suspend at all. Otherwise the compiler saves the coroutine's state in
its frame, and calls `await_suspend()`, with a
[`std::coroutine_handle`]{index="`std::coroutine_handle`"}, a pointer to
the frame through which anyone can later `resume()` the coroutine; then
the coroutine returns to whoever called or resumed it last. When it's
resumed, `await_resume()`'s result is the value of the `co_await`
expression. That's all: when and how a coroutine is resumed is the
awaiter's business, and so Seastar's.

## How Seastar builds on it {index="coroutine!in Seastar"}

`<seastar/core/coroutine.hh>` makes `seastar::future<T>` a coroutine
return type, with a promise type of its own. Its whole design is in a
few facts, each of which explains one of the earlier chapters' rules.

**The promise type is a task.** It derives from `seastar::task`, from
`<seastar/core/task.hh>`, the type of everything in the reactor's
queues, and its `run_and_dispose()`, which the reactor calls to run a
task, calls `resume()` on the coroutine's handle. So a suspended coroutine is resumed exactly as a
continuation is run: as a task, in its scheduling group
([chapter](#ch:scheduling-groups)), when something puts it in the queue.

**It starts at once, and ends at once.** Its `initial_suspend()` and
`final_suspend()` both return `std::suspend_never`: a coroutine starts
running inside its caller, without a task of its own, until it first
suspends, and when it finishes, its frame is freed right away. This
program shows the order:

```{.cpp include="examples/coroutines-inside/run-order.cc" snippet="order"}
```

```
caller: calls the callee
  callee: starts, inside its caller
caller: got its future, available: false
  callee: resumes, as a task of its own
caller: the callee is done
```

The callee's first lines ran before the call returned. That's why the
Lambda Coroutine Fiasco happens at the *first* `co_await`: until then,
the coroutine runs inside the call to the lambda, whose object is still
alive. The frame holds the lambda's
`this`, a pointer to the lambda object, and the captures are reached
through it; when the call returns, at the first suspension, the caller is
free to destroy the object, and the pointer in the frame dangles.

**The future comes from a `seastar::promise`** in the promise type:
`get_return_object()` returns its future, `co_return` sets its value,
and `unhandled_exception()` sets its exception. So any exception thrown
in a coroutine, even before its first suspension, becomes its failed
future, which is why the throwing pitfall of
[chapter](#ch:continuations) can't happen in a coroutine.

**`co_await` on a future** uses this awaiter, simplified from
`coroutine.hh`:

```cpp
bool await_ready() const noexcept {
    return _future.available() && !need_preempt();
}
void await_suspend(std::coroutine_handle<promise_type> h) noexcept {
    if (!_future.available()) {
        _future.set_coroutine(h.promise());   // resume me when resolved
    } else {
        schedule(&h.promise());               // available: just yield
    }
}
T await_resume() {
    return _future.get_available_state_ref().take();
}
```

The `need_preempt()` in `await_ready()` is why every `co_await` is a
preemption point. If the future is available and the task quota isn't
used up, the coroutine goes on without suspending, at the cost of a
check. If the quota is used up, it suspends even though the future is
available, and puts itself at the end of the queue, which lets other
tasks run. A loop of `co_await`s on available futures therefore never
stalls the shard; with
[`coroutine::without_preemption_check()`]{index="`seastar::coroutine::without_preemption_check()`"},
whose awaiter leaves the check out, it does:

```{.cpp include="examples/coroutines-inside/preempt.cc" snippet="loops"}
```

Fifty million awaits each, while a timer that should fire every
millisecond notes how late it came:

```
co_await: sum 25000000 in 107 ms; the heartbeat was up to 0.1 ms late
without_preemption_check: sum 25000000 in 100 ms; the heartbeat was up to 99.4 ms late
```

The check costs almost nothing. Leaving it out is for a loop that is
known to be short, and needs the last nanosecond.

**The frame is allocated from the shard's memory**, with `malloc()`,
which is Seastar's allocator on a shard. If it can't be allocated, the
call itself throws `std::bad_alloc`, before there's a future to fail,
and the allocation is marked as a critical section
([chapter](#ch:memory)), which the failure injector leaves alone. How
large is a frame? This program
measures the bytes that a call allocates before it first waits:

```{.cpp include="examples/coroutines-inside/run-order.cc" snippet="frames"}
```

With GCC, `small()`'s frame took 144 bytes, and `large()`'s 1,144; with
Clang, 128 and 1,128. The promise alone takes most of the small frame:
it's a task, with a `seastar::promise` inside. A local variable that lives
across a suspension takes its place in the frame, so a coroutine with a
large local array allocates it on the heap on every call, which a
function that keeps it on its stack doesn't. This allocation is what a
coroutine costs over a continuation on an available future, about eight
nanoseconds in [chapter](#ch:continuations)'s measurement.

## Seastar's awaitables, from the inside {index="awaitable"}

Each of the awaitables that earlier chapters used is a small awaiter,
which these facts explain:

- `coroutine::maybe_yield()`'s awaiter is ready unless `need_preempt()`
  is true, and otherwise schedules the coroutine, which is the whole of
  "yield if the task quota is used up".
- `coroutine::as_future()` returns, from `await_resume()`, the future
  itself, failed or not, instead of its value, so the coroutine can look
  at the failure without a `try` block.
- `coroutine::try_future()` resumes the coroutine only if the future
  succeeded. If it failed, the awaiter never resumes the coroutine: it
  passes the exception straight to the coroutine's own future, and
  destroys the frame, so the exception is never thrown, which is the
  expensive part, about 0.8 microseconds ([chapter](#ch:errors)).
- `coroutine::exception` is what [chapter](#ch:coroutines)'s
  `return_exception()` and `return_exception_ptr()` return, so
  `co_return coroutine::exception(e)` is another spelling of what that
  chapter wrote with `co_await`: it calls an overload of the promise's
  `return_value()` that sets the exception without throwing it. Prefer
  `co_await`, which works in every coroutine; `co_return` doesn't compile
  in one that returns `future<>`, since its promise has `return_void()`,
  and a promise can't have both.
- `coroutine::switch_to(group)` changes the scheduling group of the
  coroutine's task, schedules it, and returns the old group from
  `await_resume()`, as [chapter](#ch:scheduling-groups) showed.
- `coroutine::all` waits for several futures without allocating
  anything: for each one that isn't available, it builds a task in
  storage inside the awaiter itself, which the frame holds.

### Writing your own

An awaitable of your own is a class with the three methods. This one
suspends the coroutine for a while, with a timer that, when it fires,
puts the coroutine in the reactor's queue:

```{.cpp include="examples/coroutines-inside/awaitable.cc" snippet="sleep-for"}
```

`co_await sleep_for(10ms)` waits 10 milliseconds, as `sleep(10ms)` does.
The awaiter lives in the coroutine's frame while the coroutine waits, so
its timer does too, and the timer's function finds the coroutine's
promise, which is a task, through the handle. Unlike `sleep()`, it needs
no future and no promise: in 100 sleeps, `sleep_for()` allocated nothing,
and `seastar::sleep()` allocated 200 times. The timer's function only
schedules the coroutine, rather than resuming it there and then, so that
the coroutine runs as a task, in its scheduling group, with the stall
detector watching, rather than inside the timer's function.

## Generators {index="generator"}

`co_yield` makes a coroutine a *generator*: a function that produces a
sequence of values, one at a time, as its caller asks for them,
suspending after each. Seastar has an experimental generator,
[`coroutine::experimental::generator`]{index="`seastar::coroutine::experimental::generator`" .main},
from `<seastar/coroutine/generator.hh>`, whose body may also `co_await`,
so it can wait between values, for I/O, say:

```{.cpp include="examples/coroutines-inside/generators.cc" snippet="seastar-generator"}
```

Its consumer gets each value with `co_await numbers()`, which runs the
generator until its next `co_yield`. When there's no need to wait, the
two coroutines hand control directly to each other, without the reactor's
queue, but each value still costs a suspension and a resumption of each.
A *buffered* generator, which the generator's third template parameter,
a container, selects, amortizes this: its body yields a whole batch at
once, with
[`elements_of()`]{index="`seastar::coroutine::experimental::elements_of()`"},
and the consumer takes them one by one, without switching to the
generator for each:

```{.cpp include="examples/coroutines-inside/generators.cc" snippet="buffered"}
```

The generator suspends, and lets the consumer empty the buffer, when the
container is full, or when the reactor wants the CPU back, and it's
resumed only when the consumer has emptied the buffer: the two take
turns, and never overlap. While the generator waits for I/O, its
consumer waits too, even if the buffer has values; work that must
overlap needs two fibers, and a `queue` between them
([chapter](#ch:coordination)). "Full" means,
by default, that the container's `size()` has reached its `capacity()`:
for a `std::vector`, as here, whatever the vector grew to, which suits a
generator that yields batches. A generator that yields one value at a
time gets batches of a known size from a container whose capacity is
fixed, such as Seastar's `circular_buffer_fixed_capacity`, from
`<seastar/core/circular_buffer_fixed_capacity.hh>`, and
`doc/generator.md` recommends one, since it never allocates. But then a
batch yielded with `elements_of()` must fit in it: the generator doesn't
check, and `circular_buffer_fixed_capacity` doesn't either, so the
elements beyond its capacity are constructed over the oldest ones,
which aren't even destroyed. A container can also decide for
itself, with a `can_push_more()` of its own.

Both are marked experimental, in their namespace, so their interfaces may
still change; `doc/generator.md`, in Seastar's source, describes their
design.

C++23 has a generator of its own,
[`std::generator`]{index="`std::generator`"}, from `<generator>`, which
is synchronous: its body can't `co_await`, and its consumer uses it as a
range, in a plain `for` loop:

```{.cpp include="examples/coroutines-inside/generators.cc" snippet="std-generator"}
```

That suits Seastar code that never waits, such as walking a data
structure in memory. ScyllaDB's trie-based SSTable index uses it so, and
its SSTable parsers go further, with a generator type of their own,
written as coroutines that `co_yield` at each step of the parse, which
lets a parser stop wherever its input runs out, and continue when more
arrives.

## Exercises

1. Make `sleep_for` take an `abort_source&`, and end the wait at once,
   with an exception, if the abort source is triggered before the timer
   fires ([chapter](#ch:time)). Where does the subscription to the abort
   source live while the coroutine waits?

2. In `run-order.cc`, measure the frame of a coroutine that awaits a
   function that is itself a coroutine. Is the callee's frame part of the
   caller's? And what about a large local variable that isn't used after
   the first suspension?

3. Measure how much a value from Seastar's unbuffered generator costs,
   against the buffered one, for ten million small values.

4. Write `co_await on_shard(n)`, an awaitable that moves the rest of a
   coroutine to shard `n`. Why is it a bad idea, by
   [chapter](#ch:shared-nothing)'s rules? (Print the shard on which the
   coroutine's caller continues.)

5. Using `std::generator`, write a function that yields the keys of a
   `std::map` that match a prefix, and a coroutine that calls
   `coroutine::maybe_yield()` after each key that it takes from it. Why
   can't the generator yield to the reactor itself?
