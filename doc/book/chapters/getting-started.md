# Getting started {#ch:getting-started}

[Chapter](#ch:tutorial-introduction) showed what Seastar programs look like,
and assumed that Seastar was already installed. This chapter fills in what
it skipped: how to get Seastar, which version to use, and how to build
it; how to build your own programs with it; and which parts of C++ this
book assumes. Then we start writing Shoal, the program that the rest of
the book builds. Its first version does little, but it already has
everything a Seastar server needs before it serves anything: options, a
logger, and code running on every shard. The chapter ends with where to find
help.

## Getting Seastar

Seastar is distributed as source code, on GitHub:

```
$ git clone https://github.com/scylladb/seastar.git
```

At the time of writing, no Linux distribution packages Seastar, and
neither do the C++ package managers vcpkg and Conan. You build it from
source, and the next section shows how.

Seastar also makes [releases]{index="Seastar!releases"} only rarely. There have been six so far,
tagged `seastar-17.05.0`, `seastar-18.08.0`, `seastar-19.06.0`,
`seastar-20.05.0`, `seastar-22.11.0` and `seastar-25.05.0`, years apart,
while hundreds of changes are made every year between them. In practice,
the projects that use Seastar don't wait for releases. They choose a
commit of Seastar, use it until they need something newer, and then move
to a newer commit. ScyllaDB includes Seastar as a git [*submodule*]{index="submodule, git"}, which
records the exact commit that each version of ScyllaDB uses. Redpanda
keeps its own fork of Seastar on GitHub, with branches for its own
releases, into which it merges changes from Seastar's main branch, and
sends its improvements back. Either way, an application always knows
exactly which Seastar it is built with, and moves to a newer one
deliberately. The programs in this book were checked against Seastar's
main branch of November 2026.

### Backward compatibility {index="backward compatibility"}

Moving to a newer commit of Seastar is usually easy, because Seastar
promises [*source compatibility*]{index="source compatibility" .main}: an application that compiled with an
older Seastar should compile with a newer one. It must be rebuilt, though:
an application can't be linked with a Seastar other than the one it was
compiled with. The promise covers only Seastar's public names, not those
in the namespace [`seastar::internal`]{index="internal namespace"}, which you will see in Seastar's
headers but should never use. An API that Seastar replaces keeps working
for a while, and your compiler warns that it's [*deprecated*]{index="deprecation"}. Seastar also
supports only the last two C++ standards (today, C++23 and C++26) and the
two newest major versions of GCC and of Clang, so an application that must
build with an older compiler can't use a newer Seastar.
[Chapter](#ch:upgrading-seastar) explains compatibility in more detail,
including the rare changes that can't be made compatibly, and how to
upgrade.

## Building Seastar

Seastar depends on several libraries: Boost, fmt, c-ares, GnuTLS,
liburing, hwloc and others. On the common Linux distributions (Fedora,
Red Hat and its relatives, Debian and Ubuntu, and Arch), a script in
Seastar's repository installs them all from the distribution's packages:

```
$ sudo ./install-dependencies.sh
```

Then [*configure*]{index="`configure.py`" .main} the build, choosing a [*mode*]{index="build modes" .main}, and then build it:

```
$ ./configure.py --mode=release --prefix=$HOME/seastar-install
$ ninja -C build/release
```

`configure.py` is a thin wrapper around CMake, and `ninja` does the
actual build. The `--prefix` option says where the next step,
installing, will put Seastar: here, in a directory of our own, so that
installing needs no special permissions. Building Seastar takes a while,
and a lot of memory, especially with many cores compiling in parallel;
if the compiler runs out of memory, run `ninja` with a lower `-j`. On
the eight-core laptop we used (the Intel Core Ultra 7 258V of
[chapter](#ch:tutorial-introduction)), the library alone took a minute
and a half to build. Then install it:

```
$ ninja -C build/release install
```

By default, the build also compiles Seastar's tests, its demos and a few
applications, two and a half times as many files as the library itself.
You don't need them to build your own programs, and `./configure.py` can
leave them out with `--without-tests`, `--without-apps` and
`--without-demos`, which makes the build much shorter.

### Build modes {index="build modes"}

The mode chooses how Seastar is compiled. The four main modes are:

| Mode | Optimized | Checks | Use it for |
|---|---|---|---|
| `release` | yes | Seastar's own assertions | production, and measuring performance |
| `dev` | yes, but less with GCC | all assertions | the edit, compile and test cycle |
| `debug` | no | everything, including sanitizers | finding bugs, and debugging with gdb |
| `sanitize` | for size | everything, including sanitizers | finding bugs in larger tests |

Seastar checks its most important assumptions with `SEASTAR_ASSERT()`,
an assertion of its own, which stays on in every mode.[]{index="assertions"} For example,
calling `get()` on a future that isn't available yet stops the program
even in `release` mode (exercise 1 of [chapter](#ch:futures-and-promises)
shows it). The standard [`assert()`]{index="`assert()`"}, in Seastar's code and in yours, is
off in `release` mode, which defines [`NDEBUG`]{index="`NDEBUG`"}, as CMake's usual optimized
builds do. In `dev` mode, GCC [optimizes less]{index="optimization!in `dev` mode"} (`-O1` instead of `-O2`),
to compile faster. Clang uses `-O2` in `dev` mode too: when this was
decided, in 2021, Clang compiled Seastar about as fast at `-O2` as at
`-O1`, and the tests then needed half as much CPU time.

In many other projects, a [debug build]{index="debug mode" .main} is just a build without
optimization, with debugging information for gdb. Seastar's `debug` mode
is much more than that. The `release` mode has debugging information
too (it's built as CMake's `RelWithDebInfo`), so gdb works with it. What
the debug modes, `debug`
and `sanitize`, add are run-time checks, which make a program much
slower, by a factor of a hundred or more (Shoal served 150 times fewer
requests in `debug` mode than in `release` mode), but find bugs that the
other modes hide. The [sanitizers]{.index}, which both modes enable, check every
memory access, Seastar's own expensive checks are on, and Seastar
deliberately runs its tasks in an unpredictable order, and makes every
`co_await` give up the CPU, to expose code that works only by luck.
[Chapter](#ch:testing) explains what these modes do.

You build and install each mode separately, and an application must be
built in the same mode as the Seastar it uses. This book uses `release`
unless it says otherwise.

### Choosing a compiler {index="compilers"}

Seastar builds with both GCC and Clang. By default, `configure.py` uses
GCC (`g++`); to use Clang, say so:

```
$ ./configure.py --mode=release --compiler=clang++ --prefix=$HOME/seastar-install
```

Either compiler works, but if you are starting a serious project, we
recommend [Clang]{.index}. Both large Seastar applications use it. ScyllaDB moved
from GCC to Clang in 2020, when Clang's support for coroutines was more
mature than GCC's, and since then has come to rely on other parts of the
LLVM toolchain too: its linker, `lld`, its link-time and profile-guided
optimization, and its code coverage tools. Redpanda builds with a version of Clang that it pins
and downloads, so that all its developers use exactly the same compiler.
And Seastar's [continuous integration]{.index}, the automatic tests that GitHub
runs on every proposed change, builds the debug and sanitize modes only
with Clang, so those modes are better tested with it. (The reason is a
GCC bug that miscompiled some coroutines, which the sanitizers caught.
GCC 16.2 fixed it, but GCC 15 and 16.1 have it.) GCC remains a
good choice, especially since it's the default compiler of most
distributions. All the code in this book was checked with both.

Whichever you choose, build your application with the same compiler as
Seastar. Mixing them is not supported, and doesn't even get far, because
some of the compiler options that Seastar passes on to your build are
specific to the compiler that built it.

`configure.py` also chooses the [C++ standard]{index="C++ standard"}. By default, it uses the
newest one your compiler supports, today C++26 with recent compilers, and
`--c++-standard=23` asks for C++23 instead. Your own programs are then
compiled for the same standard as Seastar, without your having to choose
it: as the next section shows, Seastar tells your build which standard
to use. The code in this book uses nothing newer than C++23, unless it
says otherwise, and we checked all of it with both standards.

### When a dependency doesn't fit

Sometimes a library that your distribution provides is too old for
Seastar, or, more rarely, has a bug that Seastar can't live with. For
such cases, `configure.py` can download a known-good version of the
library, and build it as part of Seastar's build. This is called
[*cooking*]{index="cooking dependencies" .main} it. For example, when we built Seastar on Fedora 44 for this
book, `configure.py` refused the distribution's [c-ares]{.index}, a library for
asynchronous DNS lookups:

```
CMake Error at cmake/SeastarDependencies.cmake:93 (message):
  c-ares 1.34.8 is not supported: it silently freezes DNS lookups
  (https://github.com/scylladb/seastar/issues/3697).  Build with
  ./configure.py --cook c-ares to use a known-good version, or, if this
  c-ares is patched with the fix, set
  -DSeastar_ASSUME_C_ARES_DNS_FREEZE_FIXED=ON
```

The message says what to do: add `--cook c-ares` to the `configure.py`
command. You may need it on your distribution, or you may need to cook a
different library, or none. A cooked library is built and installed in
Seastar's build directory, under `build/release/_cooking/installed`, and
installing Seastar doesn't copy it to Seastar's installation directory.
So when you build your own program, you must tell the build where the
cooked libraries are, as the next section shows.

If you prefer not to install Seastar's dependencies on your computer at
all, Seastar's repository also includes the description of a Docker
container with everything needed to build it.
[Appendix](#app:building-seastar) explains how to use it, and covers
building Seastar in more detail: the rest of `configure.py`'s options,
building with CMake directly, using Seastar without installing it,
building with DPDK, and the pitfalls we met along the way.

## Building your own program {index="building programs"}

Once Seastar is installed, there are two ways to build a program that
uses it. [Chapter](#ch:tutorial-introduction) used the simplest one:
[`pkg-config`]{index="`pkg-config`"}, which tells the compiler where Seastar's headers are and
which libraries to link with. A single command builds a single-file
program:

```
$ export PKG_CONFIG_PATH=$HOME/seastar-install/lib64/pkgconfig
$ g++ -O2 hello.cc -o hello $(pkg-config --cflags --libs --static seastar)
```

(On some distributions, the directory is `lib` instead of `lib64`.)

The other way is [CMake]{index="CMake" .main}, and it's the better choice for anything bigger
than a single file. This is how Shoal is built, starting with its first
version below. Its whole build description, its `CMakeLists.txt` file,
is short:

```{.cmake include="shoal/v0/CMakeLists.txt" snippet="cmake"}
```

[`find_package(Seastar)`]{index="CMake!`find_package()`"} finds the installed Seastar and the libraries it
depends on, and linking with `Seastar::seastar` adds everything needed to
compile and link with it: the include paths, the libraries, and the C++
standard that Seastar was built for, which the program must use too. So
the file doesn't choose a standard itself. The other settings matter
too. Unless told otherwise, CMake compiles without any
[optimization]{index="optimization"}, which makes a Seastar program many times slower (Shoal
served a tenth as many requests), so we make the
default [`RelWithDebInfo`]{index="CMake!build type"}: optimized, and with debugging information, like
Seastar's own `release` mode. ([Chapter](#ch:futures-and-promises)
measures this, and explains why optimization matters so much for Seastar
code, and
[chapter](#ch:deploying-and-tuning) which further options production
builds use.) The command line can still choose another build type, with
`-DCMAKE_BUILD_TYPE`: for example `Debug`, for an unoptimized build
that's easier to follow in gdb. That doesn't make it a `debug`-mode
build: the mode is chosen when Seastar is built, and the program gets
Seastar's.

> **Draft note, for the editors:** the paragraph above assumes that
> Seastar pull request <https://github.com/scylladb/seastar/pull/3817> is
> merged. Until it is, a program built as `Debug` against a `release`-mode
> Seastar is compiled with Seastar's debug-mode definitions, which the
> library wasn't compiled with, and fails at run time (Shoal version 3
> aborts on an assertion in `data_sink::put()`). Remove this note when the
> pull request is merged.

The first line, [`cmake_minimum_required()`]{index="CMake!`cmake_minimum_required()`"}, is required: since version
4, CMake refuses to work without it. Its first version, 3.13, is the
oldest CMake that can build the file, the same as for Seastar itself; any
newer one can, including CMake 4. The second version, after the dots, is
not a limit. Each version of CMake changes some of its behavior, but
keeps the old behavior for files written for older versions, and warns
about it; the second version tells CMake that the file is ready for the
behavior of all versions up to 3.30. With only `VERSION 3.13`, recent
versions of CMake warn twice that Seastar's files look for Boost in the
old way.

Building Shoal takes two steps. First, *configure* a build directory,
here `build`: tell CMake which compiler to use and where to find
Seastar:

```
$ cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=g++ \
    -DCMAKE_PREFIX_PATH="$HOME/seastar-install;$SEASTAR_SRC/build/release/_cooking/installed"
```

Here `$SEASTAR_SRC` is where Seastar's source is. CMake doesn't use the
`pkg-config` file in `lib64/pkgconfig`: Seastar also installs a
description of itself for CMake, `lib64/cmake/Seastar/SeastarConfig.cmake`,
and `find_package(Seastar)` looks for it under each directory in
`CMAKE_PREFIX_PATH`. The second directory there is where the cooked
libraries are; if you didn't cook anything, you can leave it out. And if
you install Seastar where CMake looks by default, such as `/usr/local`,
you can leave out `CMAKE_PREFIX_PATH` altogether.

You configure each build directory only once (or again, to change a
setting such as the compiler): CMake remembers its settings, in
`build/CMakeCache.txt`. Then build the program:

```
$ cmake --build build
```

Run this command again each time you change the code: it rebuilds only
what changed, and configures the build again by itself if
`CMakeLists.txt` changed.

> **Draft note, for the editors:** this section assumes that Seastar pull
> request <https://github.com/scylladb/seastar/pull/3814> is merged. Until
> it is, configuring Shoal, which doesn't choose a C++ standard, makes
> Seastar's CMake files print a false "C++ Standard mismatch detected"
> warning (Shoal is still compiled with Seastar's standard). Remove this
> note when the pull request is merged.

> **Draft note, for the editors:** this chapter assumes that Seastar pull
> request <https://github.com/scylladb/seastar/pull/3818>, which cooks
> c-ares as a static library, is merged. Until it is, a program built
> with the `pkg-config` command above links with the cooked
> `libcares.so`, but at run time loads the distribution's, the very
> version that Seastar refused (CMake builds don't have this problem).
> This chapter used to warn about it, and appendix A listed it as a
> pitfall. Remove this note when the pull request is merged.

## The C++ this book assumes

Seastar needs at least C++23, and this book uses C++23 freely. It
assumes that you know the C++ that most C++ programmers use: classes and
templates, references and move semantics, RAII, exceptions, lambdas,
smart pointers, and the standard library's strings and containers. It
doesn't assume coroutines, which [chapter](#ch:coroutines) explains, or
any experience of asynchronous or concurrent programming. When it uses a
part of C++ that many programmers haven't met, it explains it in a
short section called a [*C++ corner*]{index="C++ corner"}. The index lists them all, under
*C++ corner*.

### Names {index="namespaces"}

Everything Seastar provides is in the `seastar` namespace. Writing
`seastar::` before every name quickly becomes tiring, so Seastar
applications usually avoid it, in one of two ways. ScyllaDB, like
Seastar's own demos, writes `using namespace seastar;` and uses Seastar's
names unqualified. Redpanda defines a short alias, `namespace ss =
seastar;`, and writes `ss::future<>` and `ss::sleep()`; its code has tens
of thousands of these `ss::`.

This book follows ScyllaDB: its code, and its text, use Seastar's names
without a prefix. To see where a name comes from, look it up in the
index: it lists each Seastar name in full, such as `seastar::sharded`,
but sorted by its short name, here under *sharded*.

There is one catch. A few of Seastar's names are also names in the
standard library: `seastar::future` and `std::future`,
`seastar::promise` and `std::promise`, and `shared_ptr` and
`make_shared` in both. With `using namespace seastar` and `using
namespace std` together, the compiler can't tell which one you mean, so
never write `using namespace std` in a Seastar program; this book
doesn't. Even without it, a call can be ambiguous, because C++ also
looks for a function in the namespaces of its arguments' types
([*argument-dependent lookup*]{.index .main}). For example,
`make_shared<std::pair<std::string, std::string>>(a, b)`, with two
`std::string` arguments, finds both `seastar::make_shared` and
`std::make_shared`, and doesn't compile. The cure is to write the
namespace: `seastar::make_shared`. A larger program, like ScyllaDB, puts
the `using` in a small header of its own, which every source file
includes; Shoal will do the same when it grows beyond one file.

> **C++ corner: errors from constrained templates.**[]{index="C++ corner!errors from constrained templates"}[]{index="concepts!error messages"} Many of
> Seastar's templates state what they accept, in a [*requires clause*]{index="requires clause" .main}
> (C++20's *concepts*), and when a call doesn't fit, the compiler's error
> quotes the clause. Suppose that the echo server of
> [chapter](#ch:tutorial-introduction) gave `handle_exception()` a
> function that takes the exception itself, `const std::exception& e`,
> instead of the `std::exception_ptr` that `handle_exception()` passes.
> Clang says (shortened):
>
> ```
> echo.cc:40:52: error: no matching member function for call to
> 'handle_exception'
> .../seastar/core/future.hh:1979:15: note: candidate template
> ignored: constraints not satisfied [with Func = (lambda at
> echo.cc:41:13)]
> .../seastar/core/future.hh:1975:14: note: because
> 'std::is_invocable_r_v<future<void>, (lambda at echo.cc:41:13),
> std::exception_ptr>' evaluated to false
> ```
>
> The first line points to the call in your code. "Constraints not
> satisfied" says that what you passed doesn't fit, not that something is
> wrong inside Seastar, and the lines that end with "evaluated to false"
> say what was needed: here, a function that can be called with a
> `std::exception_ptr`. GCC says the same in 45 lines, arranged as a tree.
> Look in it for the line of your own code, for "constraints not
> satisfied", and for the requires clause that it quotes from Seastar's
> header.

## Shoal, version 0 {index="Shoal!version 0"}

It's time to start Shoal.[^shoal-v0] As [chapter](#ch:case-for-seastar)
said, Shoal is a key-value store that speaks the Redis protocol, and by the
end of the book it stores its data on disk, on every core. First, a word
about its name.

[^shoal-v0]: The complete source of every version of Shoal is in the
    book's source, in its own directory. This chapter's version is in
    `shoal/v0/`.

> **About the name.** [*Shoal*]{index="Shoal!name of"} has one syllable, and rhymes with *goal* and
> *bowl*. A *school* of fish swims in coordination: all its fish turn
> together and head the same way, as if they followed a leader. When fish
> keep together but each one swims its own way, biologists call the group
> a *shoal*. A shoal is a good picture of a sharded server: shards that
> work independently, with no shared state and no leader, yet together
> form one service. And it swims in the same sea as Seastar, whose name
> is both the name of a sea creature and a pun on *C\**, a common
> shorthand for Cassandra.

Version 0 doesn't store or serve anything yet. It reads its options,
starts on every shard, logs what it found, and exits. But every later
version starts the same way, so it's worth looking at closely. Its code
is all in one file, `main.cc`, and each piece of code below is labeled
with the file it comes from. It begins with a [*logger*]{index="`seastar::logger`" .main}:

```{.cpp include="shoal/v0/main.cc" snippet="logger"}
```

A `seastar::logger`, from `<seastar/util/log.hh>`, has a name, here
"shoal", and methods for messages of
each [*log level*]{index="log level" .main}: `error()`, `warn()`, `info()`, `debug()` and
`trace()`. Their arguments are formatted like those of `std::format()`.
Messages of level `info` and above are printed by default; the others
only when the user asks for them, as we will see in a moment. A program
usually has one logger per subsystem, so the user can choose how much to
hear from each one. Shoal has only one, for now.

Next, a structure holding Shoal's settings, which for now is only the
port it will listen on:

```{.cpp include="shoal/v0/main.cc" snippet="options"}
```

The heart of Shoal is the function `shoal_main()`, which gets the
settings, and runs on shard 0:

```{.cpp include="shoal/v0/main.cc" snippet="shoal-main"}
```

It logs a greeting, then asks every shard to log that it is ready, with
`smp::invoke_on_all()`, which you met in
[chapter](#ch:tutorial-introduction). Each shard logs at the `debug`
level, so normally you don't see these messages. Finally, Shoal admits
that it can't do anything yet.

`main()` sets up the program, and calls `shoal_main()`:

```{.cpp include="shoal/v0/main.cc" snippet="main"}
```

There are several new things here. The `app_template::config` gives the
program a name and a description, which `--help` shows. Then
[`add_options()`]{index="`app_template::add_options()`"} declares Shoal's own command-line option, `--port`, with
its type, its default value and its help text. It adds to Seastar's
standard options, such as `-c` and `-m`, which you met in
[chapter](#ch:tutorial-introduction): Shoal accepts both kinds on the same
command line, as we will see when we run it. This uses the Boost
library's [`program_options`]{index="Boost!`program_options`"}[]{index="`program_options`|see{Boost}"}, which `app_template` uses to parse the
command line; the syntax, a series of parenthesized declarations, is
Boost's. Once `run()` has parsed the command line,
`app.configuration()` holds the values of all the options, and we copy
the ones Shoal needs into an `options` structure.

Notice that the lambda passed to `run()` is not itself a coroutine. It
reads the options, and passes them to `shoal_main()`, which is. This is
deliberate. A coroutine lambda that captures variables, as this one
captures `app`, can access its captures only as long as the lambda object
exists, and `run()` destroys the lambda soon after calling it, while the
coroutine may still be waiting. It's one of the classic pitfalls of
[coroutines]{index="coroutine!lambda captures"}, explained in [chapter](#ch:object-lifetime). Passing the
options to a separate coroutine function, by value, avoids it: a
coroutine keeps its own copy of its parameters for as long as it runs.

Build Shoal with CMake as shown above, and run it:

```
$ ./build/shoal -c2 -m1G
INFO  [shard 0:main] shoal - Shoal starting on 2 shards, port 6379
INFO  [shard 0:main] shoal - Shoal version 0 can't serve anything yet; exiting
```

Each log message shows its level, the shard that logged it, and the
logger's name, and also the date and time, which we leave out, as in
[chapter](#ch:tutorial-introduction). To see the `debug` messages too, raise the log
level of the "shoal" logger, and let's also choose a different port:

```
$ ./build/shoal -c2 -m1G --port 6400 --logger-log-level shoal=debug
INFO  [shard 0:main] shoal - Shoal starting on 2 shards, port 6400
DEBUG [shard 0:main] shoal - shard 0 is ready
DEBUG [shard 1:main] shoal - shard 1 is ready
INFO  [shard 0:main] shoal - Shoal version 0 can't serve anything yet; exiting
```

[`--logger-log-level`]{index="command-line options!`--logger-log-level`"} sets the level of individual loggers, and
[`--default-log-level`]{index="command-line options!`--default-log-level`"} sets the level of all the others. [`--help-loggers`]{index="command-line options!`--help-loggers`"}
lists all the loggers in the program: Shoal's, and those of Seastar's own
subsystems, such as "seastar" and "io", which logged the lines about
Seastar's setup that you saw in [chapter](#ch:tutorial-introduction).

`--help` shows the program's description and its own options:

```
$ ./build/shoal --help
Shoal, a Redis-compatible key-value server
Shoal options:
  -h [ --help ]         show help message
  --help-seastar        show help message about seastar options
  --help-loggers        print a list of logger names and exit
  --port arg (=6379)    TCP port to listen on
```

Seastar's own options, such as `-c`, `-m` and the logging options above,
are listed separately, by `--help-seastar`; there are almost two hundred
lines of them. [Appendix](#app:command-line-options) describes the
important ones.

Options can also be given in the [`SEASTAR_OPTIONS`]{index="`SEASTAR_OPTIONS`" .main} environment variable,
written exactly as on the command line, Seastar's options and the
program's own alike. If an option appears in both, the command line wins.
This is convenient for experiments, when you want to give the same options
to many runs, or to a program started by a script:

```
$ export SEASTAR_OPTIONS='-c2 -m1G'
$ ./build/shoal
INFO  [shard 0:main] shoal - Shoal starting on 2 shards, port 6379
```

## Getting help, and helping {index="help, getting"}

Seastar's documentation is in its Git repository, in the `doc/` directory, and
its [API reference]{.index}, generated from the comments in its headers, is online
at <https://docs.seastar.io/>. The headers themselves are often the best
reference: most public functions have a comment explaining what they do.

Seastar is developed on GitHub. Questions and discussions about using it
go to the [*Discussions*]{index="GitHub!Discussions"} tab of its GitHub page, and bugs to its [*Issues*]{index="GitHub!Issues"}.
Changes are submitted as GitHub pull requests; Seastar's `HACKING.md`
explains how to prepare one. Before reporting a bug, it's worth checking
whether it was already reported, and fixed in a newer commit than the one
you use.

## Exercises

1. Build Seastar in `release` mode and in `dev` mode, and compare how long
   the two builds take. Then build `pong` from
   [chapter](#ch:tutorial-introduction) against each of them. For the
   `dev` build, you don't need to install it: use the `pkg-config` file in
   its build directory, giving `pkg-config` its path,
   `build/dev/seastar.pc`. Compare the sizes of the two programs, and run
   both. What do you need to do to run the `dev` one? Why? (Hint: compare
   the Seastar libraries in the two build directories.)

2. Add a `--greeting` option to Shoal, a string with a default value, and
   make every shard log it at the `info` level. Then give it on the
   command line, in `SEASTAR_OPTIONS`, and in both at once with different
   values. Which value does Shoal use in each case? Can the greeting
   contain a space when it's given in `SEASTAR_OPTIONS`?

3. Run Shoal with `--logger-log-level shoal=trace`, and then with
   `--default-log-level debug`. Where do the extra messages come from in
   each case? Among the messages of the "seastar" logger, find the ones
   that say which CPU each shard runs on, and how much memory it got. Run
   Shoal with `-c2`, and then with `-c4`: which CPUs does Seastar choose,
   and how much memory does each shard get?
   Compare them with the output of `lscpu -e`. If your computer has cores
   of different speeds (see the first exercise of
   [chapter](#ch:case-for-seastar)), did Seastar choose the fast ones?

4. The first line that Seastar logs when it starts says which *reactor
   backend* it uses: the kernel interface through which it waits for
   network and disk events. Find the option that chooses it in
   `--help-seastar`, see which backends your machine offers, and run Shoal
   with each of them. One of them refuses to start without another
   option. Which one, and what does it need? ([Chapter](#ch:how-seastar-runs-your-code)
   explains what the backends do.)
