# Coroutine Generator

## Overview

The generator implementation is based on C++23 proposal [P2502R2](https://wg21.link/P2502R2). It provides
asynchronous coroutine-based value generation with two variants:

- **Unbuffered**: Zero-copy, one suspension per element
- **Buffered**: Batches elements, amortizes suspension overhead

Unlike `std::generator`, `operator()` is a coroutine returning
`std::optional<reference_type>`.

## Object Relationships and Control Flow

The generator implementation uses awaiters to manage control flow between producer and consumer coroutines. Each awaiter has multiple entry/exit points:

### yield_awaiter lifecycle (4 entry/exit points):
1. Created by `co_yield value` → `promise.yield_value()` creates awaiter
2. `co_await yield_awaiter` → calls `await_suspend(producer_handle)`
3. `await_suspend()` branches:
   - Direct path: returns `consumer_handle` (symmetric transfer)
   - Scheduler path: schedules consumer, returns `noop_coroutine()`
4. `await_resume()` called:
   - By scheduler (after scheduler path)
   - After symmetric transfer back from consumer

### next_awaiter/call_awaiter lifecycle (4 entry/exit points):
1. Created by `co_await gen()` → `operator()` creates awaiter
2. `co_await call_awaiter` → calls `await_suspend(consumer_handle)`
3. `await_suspend()` branches:
   - Direct path: returns `producer_handle` (symmetric transfer)
   - Scheduler path: schedules producer, returns `noop_coroutine()`
4. `await_resume()` called:
   - By scheduler (after scheduler path)
   - After symmetric transfer back from producer

```mermaid
graph TB
    subgraph Consumer[" "]
        C_Exec[Consumer Executing]
        C_Suspended[Consumer Suspended]
        C_Resume[Consumer await_resume#40;#41;]

        C_Exec -->|co_await gen#40;#41;| CallAwaiter[Create call_awaiter]
        CallAwaiter -->|co_await| CA_Suspend[call_awaiter::await_suspend#40;#41;]

        CA_Suspend -->|!need_preempt#40;#41;<br/>return producer_handle| P_Exec
        CA_Suspend -->|need_preempt#40;#41;<br/>schedule#40;producer#41;<br/>return noop| C_Suspended

        P_Resume -->|symmetric transfer| C_Resume
        Sched -->|resume consumer| C_Resume
        C_Resume --> C_Exec
    end

    subgraph Producer[" "]
        P_Exec[Producer Executing]
        P_Suspended[Producer Suspended]
        P_Resume[Producer await_resume#40;#41;]

        P_Exec -->|co_yield value| YieldValue[promise.yield_value#40;#41;]
        YieldValue -->|create| YieldAwaiter[Create yield_awaiter]
        YieldAwaiter -->|co_await| YA_Suspend[yield_awaiter::await_suspend#40;#41;]

        YA_Suspend -->|!need_preempt#40;#41;<br/>return consumer_handle| C_Exec
        YA_Suspend -->|need_preempt#40;#41;<br/>schedule#40;consumer#41;<br/>return noop| P_Suspended

        C_Resume -->|symmetric transfer| P_Resume
        Sched -->|resume producer| P_Resume
        P_Resume --> P_Exec
    end

    Sched[Seastar Scheduler]

    style C_Exec fill:#e1f5ff
    style P_Exec fill:#ffe1f5
    style Sched fill:#fff5e1
    style CallAwaiter fill:#d0e0ff
    style YieldAwaiter fill:#ffd0e0
```

## Control Flow

Producer and consumer transfer control via symmetric transfer when
`!need_preempt()`. When `need_preempt()` returns true, the target coroutine
is scheduled via `seastar::schedule()` and the current coroutine suspends
to `noop_coroutine()`.

The sequence diagrams below show the complete lifecycle of awaiters as
participants. Awaiters are shown with their creation and destruction
points via activate/deactivate markers. Three separate diagrams cover
the different awaiter types and their behaviors.

### Unbuffered Generator: yield_awaiter (Zero-Copy)

This is the most common case where `co_yield` passes an rvalue reference.
The promise stores only a pointer to the yielded value (zero-copy).

```mermaid
sequenceDiagram
    participant Consumer
    participant CallAwaiter as call_awaiter
    participant Producer
    participant YieldAwaiter as yield_awaiter
    participant Scheduler

    activate Consumer
    Note over Consumer: co_await gen()
    Consumer->>CallAwaiter: create call_awaiter
    activate CallAwaiter
    Note over CallAwaiter: await_ready() = false
    Note over CallAwaiter: await_suspend(consumer)

    alt !need_preempt() - symmetric transfer
        Note over CallAwaiter: return producer_handle
        CallAwaiter-->>Producer: symmetric transfer
        deactivate Consumer
        activate Producer
    else need_preempt() - via scheduler
        activate Consumer
        activate CallAwaiter
        CallAwaiter->>Scheduler: schedule(producer)
        Note over CallAwaiter: return noop_coroutine()
        deactivate Consumer
        Scheduler->>Producer: resume
        activate Producer
    end

    Note over Producer: co_yield value (rvalue)
    Note over Producer: stores pointer only
    Producer->>YieldAwaiter: create yield_awaiter
    activate YieldAwaiter
    Note over YieldAwaiter: await_ready() = false
    Note over YieldAwaiter: await_suspend(producer)

    alt !need_preempt() - symmetric transfer
        Note over YieldAwaiter: return consumer_handle
        YieldAwaiter-->>Consumer: symmetric transfer
        deactivate YieldAwaiter
        deactivate Producer
        activate Consumer
        Note over CallAwaiter: await_resume()
        CallAwaiter->>Consumer: return optional<ref>
        deactivate CallAwaiter
    else need_preempt() - via scheduler
        activate Producer
        activate YieldAwaiter
        activate CallAwaiter
        YieldAwaiter->>Scheduler: schedule(consumer)
        Note over YieldAwaiter: return noop_coroutine()
        deactivate YieldAwaiter
        deactivate Producer
        Scheduler->>Consumer: resume
        activate Consumer
        Note over CallAwaiter: await_resume()
        CallAwaiter->>Consumer: return optional<ref>
        deactivate CallAwaiter
    end
```

### Unbuffered Generator: copy_awaiter (Type Conversion)

Used when `co_yield` passes a `const` lvalue reference requiring type conversion.
The awaiter creates and stores a copy of the converted value.

```mermaid
sequenceDiagram
    participant Consumer
    participant CallAwaiter as call_awaiter
    participant Producer
    participant CopyAwaiter as copy_awaiter
    participant Scheduler

    activate Consumer
    Note over Consumer: co_await gen()
    Consumer->>CallAwaiter: create call_awaiter
    activate CallAwaiter
    Note over CallAwaiter: await_ready() = false
    CallAwaiter-->>Producer: transfer to producer
    deactivate Consumer
    activate Producer

    Note over Producer: co_yield const_lvalue
    Note over Producer: needs type conversion
    Producer->>CopyAwaiter: create copy_awaiter
    activate CopyAwaiter
    Note over CopyAwaiter: stores converted copy
    Note over CopyAwaiter: sets _value_ptr
    Note over CopyAwaiter: await_ready() = false
    Note over CopyAwaiter: await_suspend(producer)

    alt !need_preempt() - symmetric transfer
        Note over CopyAwaiter: return consumer_handle
        CopyAwaiter-->>Consumer: symmetric transfer
        deactivate CopyAwaiter
        deactivate Producer
        activate Consumer
        Note over CallAwaiter: await_resume()
        CallAwaiter->>Consumer: return optional<ref>
        deactivate CallAwaiter
        Note over Consumer: value points to copy_awaiter._value
    else need_preempt() - via scheduler
        activate Producer
        activate CopyAwaiter
        activate CallAwaiter
        CopyAwaiter->>Scheduler: schedule(consumer)
        Note over CopyAwaiter: return noop_coroutine()
        deactivate CopyAwaiter
        deactivate Producer
        Scheduler->>Consumer: resume
        activate Consumer
        Note over CallAwaiter: await_resume()
        CallAwaiter->>Consumer: return optional<ref>
        deactivate CallAwaiter
        Note over Consumer: value points to copy_awaiter._value
    end
```

### Buffered Generator: yield_awaiter (Batching)

The buffered generator accumulates elements and conditionally suspends.
`await_ready()` can return `true` when the buffer has space and no preemption is needed.

```mermaid
sequenceDiagram
    participant Consumer
    participant CallAwaiter as call_awaiter
    participant Producer
    participant YieldAwaiter as yield_awaiter
    participant Scheduler

    activate Consumer
    Note over Consumer: co_await gen()
    Consumer->>CallAwaiter: create call_awaiter
    activate CallAwaiter
    Note over CallAwaiter: buffer empty?
    Note over CallAwaiter: await_ready() = false
    Note over CallAwaiter: await_suspend(consumer)
    Note over CallAwaiter: clear buffer
    CallAwaiter-->>Producer: transfer to producer
    deactivate Consumer
    activate Producer

    Note over Producer: co_yield element
    Note over Producer: push to buffer
    Producer->>YieldAwaiter: create yield_awaiter
    activate YieldAwaiter
    Note over YieldAwaiter: should_suspend?
    Note over YieldAwaiter: !can_push_more OR need_preempt

    alt await_ready() = true (buffer has space, !need_preempt)
        Note over YieldAwaiter: continue without suspend
        deactivate YieldAwaiter
        Note over Producer: co_yield next element
        Note over Producer: ...continues batching
    else await_ready() = false (buffer full OR need_preempt)
        activate Producer
        activate CallAwaiter
        activate YieldAwaiter
        Note over YieldAwaiter: await_suspend(producer)
        alt !need_preempt() - symmetric transfer
            Note over YieldAwaiter: return consumer_handle
            YieldAwaiter-->>Consumer: symmetric transfer
            deactivate YieldAwaiter
            deactivate Producer
            activate Consumer
            Note over CallAwaiter: await_resume()
            CallAwaiter->>Consumer: return buffer[0]
            deactivate CallAwaiter
            Note over Consumer: drain buffer without suspension
            Note over Consumer: buffer[1], buffer[2], ...
        else need_preempt() - via scheduler
            activate Producer
            activate YieldAwaiter
            activate CallAwaiter
            YieldAwaiter->>Scheduler: schedule(consumer)
            Note over YieldAwaiter: return noop_coroutine()
            deactivate YieldAwaiter
            deactivate Producer
            Scheduler->>Consumer: resume
            activate Consumer
            Note over CallAwaiter: await_resume()
            CallAwaiter->>Consumer: return buffer[0]
            deactivate CallAwaiter
            Note over Consumer: drain buffer without suspension
        end
    end
```

## Unbuffered Generator

### Characteristics

- Stores pointer to value in producer's stack frame
- No copies or moves
- One suspension per yielded element

### Usage

```cpp
generator<const T&> produce() {
    T value;
    co_yield value;  // Zero-copy: stores pointer only
}
```

Use when element moves are expensive or latency is critical.

## Buffered Generator

### Characteristics

- Accumulates elements in a container
- Suspends when buffer full or need_preempt() returns true
- Consumer drains buffer without suspensions

### Usage

```cpp
generator<const T&, T, circular_buffer_fixed_capacity<T, 128>> produce() {
    co_yield element;           // Individual elements
    co_yield std::span(data);   // Ranges
}
```

Use when throughput matters and element moves are cheap.

### Buffer Measurement

The buffered variant uses a customization point object to check buffer capacity:

```cpp
// Priority 1: Member function
struct MemoryBuffer {
    bool can_push_more() const {
        return memory_used < memory_limit;
    }
};

// Priority 2: ADL free function
namespace my_ns {
    bool can_push_more(const MyContainer& c);
}

// Priority 3: Default
return container.size() < container.capacity();
```

## Lifetime Guarantees

### Unbuffered

When `co_yield` evaluates an expression producing a glvalue, the object lives
until the coroutine resumes. The promise stores only a pointer.

### Buffered

Values are moved into the buffer and have independent lifetime.

## Exception Handling

Exceptions in the producer are caught by `promise_type::unhandled_exception()`
and stored. On the next consumer resumption, `await_resume()` rethrows the
exception.

## Template Parameters

### Single-parameter form

```cpp
generator<int>              // value_type=int, reference=int&&
generator<const string&>    // value_type=string, reference=const string&
```

### Two-parameter form

```cpp
generator<string_view, string>  // Return string_view, store string
```

Allows proxy reference pattern: producer yields `string`, consumer receives
`string_view`.

## Performance

Performance characteristics measured using benchmarks in `tests/perf/coroutine_perf.cc`.

### Benchmark Results

Each test generates 100 integers per iteration (Release mode with `-O2`):

| test                                |           runtime |    allocs |      inst |    cycles |
| -                                   |                -: |        -: |        -: |        -: |
| coroutine_test.unbuffered_generator |  308ns ± 0.15%    |     2.000 |   9356.47 |    1639.1 |
| coroutine_test.buffered_generator   |  200ns ± 0.14%    |     2.000 |   6259.26 |    1069.6 |

**Unbuffered generator:**
- One suspension per element (100 suspensions per iteration)
- Zero-copy: stores pointer to value in producer's stack frame
- ~3.1ns per element (308ns / 100)

**Buffered generator** (using `circular_buffer_fixed_capacity<int, 16>`):
- Amortized suspensions (~6-7 suspensions per iteration with buffer size 16)
- Moves elements into fixed-capacity buffer (zero heap allocations)
- **35% faster than unbuffered** due to amortized suspension overhead
- ~2.0ns per element (200ns / 100)

**Important: Container choice is critical for buffered generator performance!**

Using `std::vector` instead of `circular_buffer_fixed_capacity` results in:
- **2.6x slower performance** (524ns vs 200ns)
- **2x more allocations** (4 vs 2) due to dynamic memory
- **2x more instructions** (13,088 vs 6,259)

The heap allocation overhead completely negates the batching benefit. Always use fixed-capacity containers like `circular_buffer_fixed_capacity` for best performance.

### Choosing Between Variants

The choice depends on multiple factors:

**Use unbuffered when:**
- Element moves are expensive (large objects, non-trivial move constructors)
- Latency is critical (need first element ASAP)
- Memory pressure is a concern (no buffering overhead)
- Elements are naturally references to existing data

**Use buffered when:**
- Throughput matters more than latency
- Elements are cheap to move (integers, small PODs)
- Producer can generate elements in batches
- Using a fixed-capacity container (avoids heap allocations)

**Note:** The buffered variant's performance advantage becomes more pronounced with:
- Larger element counts (more amortization)
- Fixed-capacity containers like `circular_buffer_fixed_capacity` (no allocations)
- Higher element generation cost in the producer
- Natural batching in the data source
