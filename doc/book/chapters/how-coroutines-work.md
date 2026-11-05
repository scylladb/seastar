# How coroutines work {#ch:how-coroutines-work}

The earlier chapters gave rules for coroutines, without showing where
the rules come from. Every `co_await` is a
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
compiler does three things:

1. It moves the function's state off the stack, into a *frame* that it
   allocates. The frame holds the parameters, the local variables that
   live across a suspension, and which `co_await` the coroutine stopped
   at, so that it can resume from there. The function can then return to
   its caller in the middle, and continue later, when something else
   resumes it, such as the reactor, which runs it as a task. A resumed
   coroutine runs on the stack of whatever resumed it, which holds the
   functions it calls and the values that don't live across a
   suspension; everything that must survive a suspension is in the
   frame.

2. It asks the coroutine's return type what to do, through a *promise
   type*, which `std::coroutine_traits<R, Args...>::promise_type` names,
   and which is usually the return type's own nested `promise_type`.
   Despite its name, a promise type isn't a `seastar::promise`
   ([chapter](#ch:futures-and-promises)), though Seastar's, as we'll see,
   contains one. The frame holds an object of the promise type, the
   *promise object*, and the compiler turns the body, roughly, into
   this:

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

3. It turns each `co_await expr` into a call of three methods of an
   *awaiter*, which `expr` either is or provides. `await_ready()` says
   whether the result is already there; if it is, the coroutine doesn't
   suspend at all. Otherwise the compiler saves the coroutine's state in
   its frame, and calls `await_suspend()`, with a
   [`std::coroutine_handle`]{index="`std::coroutine_handle`"}, a pointer
   to the frame through which anyone can later `resume()` the coroutine;
   then the coroutine returns to whoever called or resumed it last. When
   it's resumed, `await_resume()`'s result is the value of the
   `co_await` expression.

That's all: when and how a coroutine is resumed is the awaiter's
business, and so Seastar's.

## How Seastar builds on it {index="coroutine!in Seastar"}

`<seastar/core/coroutine.hh>` makes `seastar::future<T>` a coroutine
return type, by specializing `std::coroutine_traits` for it, with a
promise type of its own. Here it is, simplified from `coroutine.hh`
(the version for `future<>` has `return_void()` instead of
`return_value()`):

```cpp
template <typename T>
class promise_type final : public seastar::task {
    seastar::promise<T> _promise;
public:
    seastar::future<T> get_return_object() noexcept {
        return _promise.get_future();
    }
    std::suspend_never initial_suspend() noexcept { return {}; }
    std::suspend_never final_suspend() noexcept { return {}; }
    template <typename U>
    void return_value(U&& value) noexcept {
        _promise.set_value(std::forward<U>(value));
    }
    void unhandled_exception() noexcept {
        _promise.set_exception(std::current_exception());
    }
    void run_and_dispose() noexcept override {
        std::coroutine_handle<promise_type>::from_promise(*this).resume();
    }
};
```

Its whole design is in a few facts, each of which explains one of the
earlier chapters' rules.

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

**The promise type contains a `seastar::promise`.**
`get_return_object()` returns this promise's future, `co_return` sets
its value, and `unhandled_exception()` sets its exception. So any exception thrown
in a coroutine, even before its first suspension, becomes its failed
future, which is why the throwing pitfall of
[chapter](#ch:continuations) can't happen in a coroutine.

**`co_await` on a future** calls Seastar's `operator co_await`, which
moves the future into an awaiter, simplified here from `coroutine.hh`:

```cpp
template <typename T>
auto operator co_await(future<T>&& f) noexcept {
    return awaiter<T>(std::move(f));
}

template <typename T>
class awaiter {
    future<T> _future;                        // the future awaited
public:
    explicit awaiter(future<T>&& f) noexcept : _future(std::move(f)) {}
    bool await_ready() const noexcept {
        return _future.available() && !need_preempt();
    }
    void await_suspend(std::coroutine_handle<promise_type> h) noexcept {
        if (!_future.available()) {
            _future.set_coroutine(h.promise()); // resume me when resolved
        } else {
            schedule(&h.promise());             // available: just yield
        }
    }
    T await_resume() {
        return _future.get_available_state_ref().take();
    }
};
```

The `need_preempt()` in `await_ready()` is why every `co_await` is a
preemption point. If the future is available and the task quota isn't
used up, the coroutine goes on without suspending, at the cost of a
check. If the quota is used up, it suspends even though the future is
available, and puts itself at the end of the queue, which lets other
tasks run. A loop of `co_await`s on available futures therefore never
stalls the shard. But the same loop with
[`coroutine::without_preemption_check()`]{index="`seastar::coroutine::without_preemption_check()`"},
whose awaiter leaves the check out, does stall it. This program has both
loops:

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
which is Seastar's allocator on a shard. Seastar treats the frame like
the task that `then()` allocates, as glue that mustn't fail: a coroutine
that cleans up, for example, and fails to start, would leave the
program in an inconsistent state. So the frame is allocated in a
critical allocation section ([chapter](#ch:memory)), which the failure
injector leaves alone, and if it can't be allocated, the program
crashes. How
large is a frame? This program
measures the bytes that a call allocates before it first waits:

```{.cpp include="examples/coroutines-inside/run-order.cc" snippet="frames"}
```

With GCC, `small()`'s frame took 144 bytes, and `large()`'s 1,144; with
Clang, 128 and 1,128. The promise object alone takes most of the small
frame: it's a task, with a `seastar::promise` inside. A local variable that lives
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
- `coroutine::exception` is the type that both `return_exception()`
  and `return_exception_ptr()`, from [chapter](#ch:coroutines), return.
  So a coroutine can also write `co_return coroutine::exception(e)`,
  instead of that chapter's `co_await`. The `co_return` calls an
  overload of the promise object's `return_value()` that sets the
  exception without throwing it. Prefer
  `co_await`, which works in every coroutine; `co_return` doesn't compile
  in one that returns `future<>`, since its promise type has
  `return_void()`, and a promise type can't have both.
- `coroutine::switch_to(group)` changes the scheduling group of the
  coroutine's task, schedules it, and returns the old group from
  `await_resume()`, as [chapter](#ch:scheduling-groups) showed.
- `coroutine::all` waits for several futures without allocating
  anything: for each one that isn't available, it builds a task in
  storage inside the awaiter itself, which the frame holds.

### Writing your own

An awaitable of your own is a class with the three methods. The
following example suspends the coroutine for a while, with a timer that, when it fires,
puts the coroutine in the reactor's queue:

```{.cpp include="examples/coroutines-inside/awaitable.cc" snippet="sleep-for"}
```

`co_await sleep_for(10ms)` waits 10 milliseconds, as
`co_await sleep(10ms)` does.
The awaiter lives in the coroutine's frame while the coroutine waits, so
its timer does too, and the timer's function finds the coroutine's
promise object, which is a task, through the handle. Unlike `sleep()`,
it needs no future and no `seastar::promise`: in 100 sleeps, `sleep_for()` allocated nothing,
and `seastar::sleep()` allocated 200 times. The timer's function
doesn't resume the coroutine itself: it only puts the coroutine, which
is a task, in the reactor's queue. The reactor calls the functions of
all the timers that expired one after another, each in its timer's
scheduling group, so a timer's function should be short. Had it resumed
the coroutine, the rest of the coroutine, however long, would have run
inside the timer's function, in the timer's scheduling group instead of
the coroutine's own, while the other timers waited. Put in the queue,
the coroutine runs later, as a task of its own scheduling group, like
any other.

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

The generator and its consumer take turns, and never run at the same
time. The generator runs, filling the buffer, until the buffer is full,
or until the reactor wants the CPU back. Then it suspends, and the
consumer takes the values from the buffer, one by one. The generator is
resumed, to fill the buffer again, only when the buffer is empty. So
while the generator waits, for I/O for example, its consumer waits too,
even if the buffer still has values. Work that must overlap needs two
fibers, with a `queue` between them ([chapter](#ch:coordination)).

By default, the buffer is full when the container's `size()` reaches its
`capacity()`. A `std::vector`'s capacity is whatever the vector grew to,
which suits a generator that yields batches, as in this example. A
generator that yields one value at a time needs a container whose
capacity is fixed, to get batches of a known size: Seastar's
`circular_buffer_fixed_capacity`, from
`<seastar/core/circular_buffer_fixed_capacity.hh>`, which
`doc/generator.md` recommends, since it never allocates. A container can
also decide for itself when it's full, with a `can_push_more()` method
of its own.

Don't mix the two styles: with a container of fixed capacity, a batch
yielded with `elements_of()` must fit in the container. Neither the
generator nor the container checks, so the elements
beyond the capacity are constructed over the oldest ones, which aren't
even destroyed.

The generator and `elements_of()` are both marked experimental, in their
namespace, so their interfaces may still change; `doc/generator.md`, in Seastar's source, describes their
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

1. Make `sleep_for` take an `abort_source&`, and end the wait immediately,
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
