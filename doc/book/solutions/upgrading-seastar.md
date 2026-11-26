# Upgrading Seastar {#sol:upgrading-seastar}

These answers were found on November 11, 2026, in Seastar's main branch
as it was then, at commit 04b1c04dc. Yours will differ in the details,
which is part of the point.

## Exercise 1

`git log --since='3 months ago'` listed 214 commits, 185 of them not
merges, by 33 different people. The most active five made more than
half of them: 54, 37, 27, 21 and 18 commits.

`git log -S'[[deprecated' --since='3 months ago'` listed six commits.
Three added `[[deprecated]]` attributes:

- `httpd: let a server be given a socket to serve`, which deprecated
  three of `http_server`'s functions, `do_accepts()` (two overloads) and
  `accept_loop()`, with the message "listen() starts accepting by itself;
  to serve a socket you created, use listen(server_socket&&)".
- `httpd: deprecate http_server_tester`, whose message is "Use
  http_server::listen(server_socket&&) to serve a socket of your own,
  and http_server::listening_addresses() to read back what is served".
- `rpc: Deprecate the per-verb scheduling group`, which deprecated the
  overload of `register_handler()` that takes a scheduling group: "use
  per-connection isolation (client_options::isolation_cookie) instead".

The other three removed APIs that had been deprecated earlier: the
`scollectd` metrics backend, an overload of `read_via_type_marker()`
that took a `boost::type<>`, and two coroutine helpers,
`make_exception()` and the `exception_ptr` overload of
`return_exception()`, deprecated in 2022 and 2024 in favor of
`coroutine::return_exception_ptr()` and `coroutine::exception`. `-S` finds a change in the number
of occurrences of the text, so it finds removals as well as additions.

Every new message names the replacement, so a user who sees the
warning knows what to write instead, without searching: the API to call,
and, for the HTTP server, why the old functions are no longer needed
(`listen()` now starts accepting by itself). That's Seastar's rule for
deprecations, and it's what makes them cheap to follow. The removals
show the other half of the cycle: what was deprecated is eventually
gone, so a program that ignores the warnings breaks when it upgrades
past the removal.

## Exercise 2

`grep -rn SEASTAR_API_LEVEL include/seastar` found 29 places, most of
them in `iostream.hh`, `future.hh` and `reactor.hh`. One that compares
it with 10 is `promise<T>::set_value()`, in `future.hh`. Below level 10,
it's a template that accepts any arguments, and builds the value from
them:

```cpp
    template <typename... A>
    void set_value(A&&... a) noexcept {
        internal::promise_base_with_type<T>::set_value_loose_type_conversion(std::forward<A>(a)...);
    }
```

which constructs the value with `T(std::forward<U>(vs)...)`, a call
that may use an `explicit` constructor. At level 10, `set_value()`
accepts only the declared type, or one implicitly convertible to it,
the way a plain `return` does. The same switch, in `coroutine.hh`, does
this for `co_return`.

The difference shows with a type whose constructor is `explicit`, such
as `std::chrono::seconds`:

```cpp
future<std::chrono::seconds> timeout() {
    co_return 42;
}

future<std::chrono::seconds> timeout_promise() {
    promise<std::chrono::seconds> p;
    p.set_value(42);
    return p.get_future();
}
```

At level 10, both are errors, "could not convert 'int' to
'std::chrono::duration<long int>'", just as `return 42;` would be in a
function returning `std::chrono::seconds`. At level 9, both compile: 42
silently becomes 42 seconds, which may be what was meant, or 42
milliseconds that someone forgot to convert. The commit that made the
change, "core/{coroutine,future}: make `co_return` more strict with
types", of March 2026, explains it with exactly such cases: a
strongly-typed value built from its underlying type, an unrelated enum
with the same underlying type, and "obscure conversions, such as a
number to `std::vector`".

`doc/compatibility.md`'s table agrees: level 10, introduced in 2026-04,
is "co_return and set_value strict type semantics", and its "Mandatory"
column is empty, so the older levels are still supported, and a
program can still ask for level 9 while it fixes its code.

## Exercise 3

```cpp
future<int> f() {
    co_return 42;
}

future<int> g() {
    co_return f();
}
```

At level 10, GCC said:

```
error: no matching function for call to 'seastar::internal::coroutine_traits_base<int>::promise_type::return_value(seastar::future<int>)'
  * error: could not convert 'seastar::future<int>' to 'int'
```

A coroutine that returns `future<int>` must `co_return` an `int`, and a
`future<int>` isn't one. With `-DSEASTAR_API_LEVEL=9` added after
`pkg-config`'s flags, it compiled, with two warnings: that
`SEASTAR_API_LEVEL` was redefined (`pkg-config` had already defined it
as 10, and a later `-D` wins), and

```
warning: 'void seastar::internal::coroutine_traits_base<T>::promise_type::return_value(seastar::future<T>&&) [with T = int]' is deprecated: Forwarding coroutine returns are deprecated as too dangerous. Use 'co_return co_await ...' until explicit syntax is available.
```

Below level 10, `co_return f();` meant "forward `f()`'s result to my
caller": the coroutine's promise took its result from `f()`'s future
when it became available. Level 10 removes that overload, and the
warning at level 9 says why and what to write instead: `co_return
co_await f();`, which waits for `f()` in the coroutine itself, so that
its exceptions and its lifetime are the coroutine's, like any other
`co_await`.

## Exercise 4

`grep -rn experimental include/seastar` found, besides the chapter's
four, WebSocket support, `fsnotifier`, the coroutine generator and
`file::experimental_list_directory()`:

- `seastar::experimental::process`, `spawn_parameters`,
  `spawn_process()` and `make_pipe()`, in `util/process.hh` and
  `core/seastar.hh`. They aren't experimental APIs any more, but what's
  left of one: Seastar moved its API for running subprocesses out of
  `experimental` in March 2026 (commit "util/process: graduate process
  management API from experimental"), and kept the old names as
  deprecated aliases, so that code that used them still compiles, with
  a warning. Unlike the HTTP client's move, which the chapter describes,
  this one didn't break anyone.
- Features that Seastar itself calls experimental, without a namespace:
  the Prometheus exporter's protobuf format (`allow_protobuf`, "protobuf
  support is experimental and off by default"), and native histograms,
  which are an experimental feature of Prometheus rather than of
  Seastar.
- `std::experimental::pmr`, in `util/std-compat.hh`, which is the
  standard library's experimental namespace, used only with a standard
  library that lacks `<memory_resource>`; it isn't a Seastar API.

So the namespaces don't tell the whole story: an API can be experimental
by its documentation alone, and an `experimental` namespace can hold
names that are only on their way out. The search is worth repeating at
each upgrade, together with the deprecation warnings.
