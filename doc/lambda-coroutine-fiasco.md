# The Lambda Coroutine Fiasco

Lambda coroutines and Seastar APIs that accept continuations interact badly. This
document explains the bad interaction and how it is mitigated.

## Lambda coroutines revisited

A lambda coroutine is a lambda function that is also a coroutine due
to the use of the coroutine keywords (typically co_await). A lambda
coroutine is notionally translated by the compiler into a struct with a
function call operator:

```cpp
[captures] (arguments) -> seastar::future<> {
    body
    co_return result
}
```

becomes (more or less)

```cpp
struct lambda {
    captures;
    seastar::future<> operator()(arguments) const {
        body
    }
};
```

## Lambda coroutines and coroutine argument capture

Coroutines, like lambdas, can capture variables from their enclosing
scope. Additionally, coroutines capture their arguments, which can occur
by value or by reference, depending on the argument's declaration.

The lambda's captures however are captured by reference. To understand why,
consider that the coroutine translation process notionally transforms a member function
(`lambda::operator()`) to a free function:

```cpp
// before
seastar::future<> lambda::operator()(arguments) const;

// after
seastar::future<> lambda_call_operator(const lambda& self, arguments);
```

This transform means that the lambda structure, which contains all the captured variables,
is itself captured by the coroutine by reference.

## Interaction with Seastar APIs accepting continuations

Consider a Seastar API that accepts a continuation, such as
`seastar::future::then(Func continuation)`. The behavior
is that `continuation` is moved or copied into a private memory
area managed by `then()`. Sometime later, the continuation is
executed (`Func::operator()`) and the memory area is freed.
Crucially, the memory area is freed as soon as `Func::operator()`
returns, which can be before the future returned by it becomes
ready. However, the coroutine can access the lambda captures
stored in this memory area after the future is returned and before
it becomes ready. This is a use-after-free.

## Solution

The solution is to avoid copying or moving the lambda into
the memory area managed by `seastar::future::then()`. Instead,
the lambda spends its life as a temporary. We then rely on C++
temporary lifetime extension rules to extend its life until the
future returned is ready, at which point the captures can longer
be accessed.

```cpp
    co_await seastar::yield().then(seastar::coroutine::lambda([captures] () -> future<> {
        co_await seastar::coroutine::maybe_yield();
        // Can use `captures` here safely.
    }));
```

`seastar::coroutine::lambda` is very similar to `std::reference_wrapper` (the
only difference is that it works with temporaries); it can be safely moved to
the memory area managed by `seastar::future::then()` since it's only used
to call the real lambda, and then is safe to discard.

## Alternative solution when lifetime extension cannot be used.

If the lambda coroutine is not co_await'ed immediately, we cannot rely on
lifetime extension and so we must name the coroutine and use `std::ref()` to
refer to it without copying it from the coroutine frame:

```cpp
    auto a_lambda = [captures] () -> future<> {
        co_await seastar::coroutine::maybe_yield();
        // Can use `captures` here safely.
    };
    auto f = seastar::yield().then(std::ref(a_lambda));
    co_await std::move(f);
```

