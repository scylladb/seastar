# Upgrading Seastar {#ch:upgrading-seastar}

[Chapter](#ch:getting-started) suggested building an application with a
specific commit of Seastar, recorded in a git submodule or in a fork, and
moving to a newer one deliberately. An application lives for years, and
moves to newer commits many times during its life. This short chapter
explains what may change from one commit of Seastar to the next, which
of those changes keep your code working and which may not, and how to
upgrade.

## Compatibility {index="backward compatibility"}

Seastar promises [*source compatibility*]{index="source compatibility" .main}: an application that compiled
with an older Seastar should compile with a newer one, and work the same.
It also keeps the [*protocols*]{index="protocol compatibility"} that it implements compatible, such as the
messages of its RPC framework ([chapter](#ch:rpc)), so that two servers,
one built with an older Seastar and one with a newer one, can talk to each
other. That matters for a cluster, which is upgraded one server at a
time, and runs both versions in the meantime.

What Seastar doesn't promise is [*link compatibility*]{.index .main}. An application must
be compiled with the same Seastar that it links with. After an upgrade,
rebuild all of the application, and every library of its own that uses
Seastar.

The promise has three more limits. It covers only Seastar's public names,
and it doesn't cover APIs that Seastar calls experimental, or compilers
and C++ standards that Seastar no longer supports. The next sections
explain each, and how Seastar changes its API when a change can't be made
compatibly.

## Public and internal names {index="internal namespace"}

Seastar's headers declare many things that applications shouldn't use:
they are there only because Seastar's own templates and inline functions
need them. Those are in the namespace `seastar::internal`, and may change
or disappear at any time. You will see them in the headers, and your
editor will offer to complete them, but code that uses them may stop
compiling after any upgrade.

Some older names became public by accident, before the `internal`
namespace existed. They aren't protected either. Seastar documents its public API
with Doxygen comments in its headers, so a name with no documentation
deserves suspicion, though not every public name is documented. When in
doubt, ask on Seastar's mailing list.

## Deprecation {index="deprecation"}

When an API turns out to be a mistake, Seastar doesn't remove it at once.
It adds a better API, and marks the old one with C++'s `[[deprecated]]`
attribute, with a message that says what to use instead. The old API keeps
working for a transition period, longer for APIs that many applications
use, and is then removed.

For example, Seastar's template for running a service on every shard,
which [Part](#part:many-cores) uses, was called `distributed<T>` until
2015, when it was renamed `sharded<T>`. The old name remained as an alias,
and ten years later, in 2025, it was deprecated. A program that still uses
it compiles, but GCC warns:

```
warning: 'using seastar::distributed = class seastar::sharded<counter>' is
deprecated: Use sharded<T> from sharded.hh instead [-Wdeprecated-declarations]
```

Fix such warnings soon after an upgrade, while the old API still exists,
and you can change your code at your own pace. If your application is
built with `-Werror`, which turns every warning into an error, a new
deprecation stops the build. Adding `-Wno-error=deprecated-declarations`
keeps these warnings as warnings, while all others remain errors.

## API levels {index="API level"}

Rarely, an API must change in a way that can't be compatible: for
example, when a function must return a different type, or when a
template must behave differently. Old code would break, and a deprecation
can't help, because the old and new API can't both exist under the same
name. For these changes, Seastar has [*API levels*]{index="API level" .main}.

Each such change increases the API level, and for a while Seastar keeps
both versions of the API, the old one in effect when an application asks
for the previous level. So an application can upgrade Seastar first,
still asking for the old level, then change its code, and only then move
to the new level. The level is chosen when Seastar is configured, with
`configure.py`'s `--api-level` option, or with the CMake variable
`Seastar_API_LEVEL` ([appendix](#app:building-seastar)), and it applies to
Seastar and to the application compiled with it, since Seastar's
`pkg-config` file and CMake package pass it on. When the transition
period ends, the old API is removed, and the new level becomes mandatory.

There have been ten levels so far. Some of the changes they made:

| Level | Year | The change |
|:---:|:---:|---|
| 6 | 2020 | A future holds one value, where before it could hold several |
| 7 | 2023 | CPU scheduling groups and I/O priority classes become one ([chapter](#ch:scheduling-groups)) |
| 9 | 2025 | A new interface for implementing data sinks, the destinations of output streams |
| 10 | 2026 | `co_return` and `set_value()` convert their argument to the result type more like a `return` statement does |

At the time of writing, level 10 is the latest, and `configure.py` asks
for it by default; CMake, configured directly, still defaults to 9, so
pass `-DSeastar_API_LEVEL=10` ([appendix](#app:building-seastar)). Level 7 became mandatory in 2024, so an application can
still ask for levels 7 to 9. This book uses level 10 throughout.

Level 10 shows how a level is used. Before it, a coroutine that returns
`future<int>` could `co_return` a `future<int>`, and its own future would
then get that future's result. This turned out to be dangerous, and was
deprecated, with a message suggesting `co_return co_await f()` instead.
Level 10 removed it: with level 10, `co_return f()` doesn't compile at
all. An application that still had such code could stay at level 9 while
it fixed it, guided by the deprecation warnings.

## Experimental APIs {index="experimental APIs"}

A few newer APIs are in namespaces called `experimental`. At the time of
writing, they are WebSocket support, in `seastar::experimental::websocket`;
`fsnotifier`, which reports changes to files, in `seastar::experimental`;
the coroutine generator, in `seastar::coroutine::experimental`; and
`file::experimental_list_directory()`, which uses it. These APIs are still
being designed, and may change incompatibly, without deprecation or an
API level.

When an experimental API settles, it moves out of the `experimental`
namespace. Its old name may remain for a while, as a deprecated alias, as
it did in 2026 for `spawn_process()`, which runs other programs; or it may
just disappear, as it did earlier the same year, when the HTTP client left
`http::experimental` after three years there, and code that used the old
name stopped compiling. Use experimental APIs when you need them, but
expect to change your code when you upgrade.

## Compilers and standards {index="compilers"}

Seastar supports the last two C++ standards, today C++23 and C++26, and
the two newest major versions of GCC and of Clang. When a new standard or
compiler is released, Seastar drops support for the oldest. So
upgrading Seastar may also require upgrading
your compiler, and an application that must keep building with an older
compiler can't keep upgrading Seastar.

## Upgrading, step by step {index="upgrading Seastar"}

Putting this together, an upgrade goes like this:

1. **See what changed.** `git log OLD..NEW` in Seastar's repository lists
   the commits between the two versions; `git log OLD..NEW -- include/`
   only those that changed the public headers. Look for deprecations
   (`git log -S'[[deprecated' OLD..NEW`), for changes of the API level,
   and for changes to experimental APIs that you use.
2. **Update and rebuild.** Move the submodule, or your fork, to the new
   commit, rebuild Seastar, and rebuild all of your application.
3. **Fix what broke,** which should be only uses of internal names,
   accidentally public names, and experimental APIs. Then fix the
   deprecation warnings.
4. **Test,** including in debug mode, which catches more mistakes
   ([chapter](#ch:testing)). A new Seastar may also behave differently
   where your code depended on something that Seastar never promised, such
   as the order in which tasks run.
5. **Move to a new API level,** if there is one, when your code is ready
   for it: before the old level is removed.

Upgrading often, in small steps, is easier than upgrading rarely, in big
ones: each step brings fewer changes, and deprecated APIs are still there
when you meet their warnings.

## Exercises

1. Look at the commits in Seastar's main branch in the last three months
   (`git log --since='3 months ago'`). How many are there, and how many
   different people made them? Find the commits that added a
   `[[deprecated]]` attribute (`git log -S'[[deprecated'` finds the
   commits that added or removed the text), and read the message that each
   attribute gives. What does it tell users to do?

2. Find the places in Seastar's headers that depend on the API level
   (`grep -rn SEASTAR_API_LEVEL include/seastar`). Pick one that compares
   it with 10, and find out what the older levels do differently. Then
   check the change against the table of API levels in Seastar's
   `doc/compatibility.md`.

3. Write a coroutine that returns `future<int>` and does
   `co_return f();`, where `f()` returns a `future<int>`. Compile it
   against Seastar at level 10, and read the error. Then compile it again,
   adding `-DSEASTAR_API_LEVEL=9` after `pkg-config`'s flags, which is
   enough to see what the headers would do at level 9 (but not to link the
   program: the library itself was built at level 10). What does the
   compiler say now?

4. List all of Seastar's experimental APIs, by searching its headers for
   `experimental`. Do you find any that this chapter missed?
