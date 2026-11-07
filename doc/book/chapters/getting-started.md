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

Seastar also makes releases only rarely. There have been six so far,
tagged `seastar-17.05.0`, `seastar-18.08.0`, `seastar-19.06.0`,
`seastar-20.05.0`, `seastar-22.11.0` and `seastar-25.05.0`, years apart,
while hundreds of changes are made every year between them. In practice,
the projects that use Seastar don't wait for releases. They choose a
commit of Seastar, use it until they need something newer, and then move
to a newer commit. ScyllaDB includes Seastar as a git *submodule*, which
records the exact commit that each version of ScyllaDB uses. Redpanda
keeps its own fork of Seastar on GitHub, with branches for its own
releases, into which it merges changes from Seastar's main branch, and
sends its improvements back. Either way, an application always knows
exactly which Seastar it is built with, and moves to a newer one
deliberately. The programs in this book were checked against Seastar's
main branch of November 2026.

### What Seastar promises

Moving to a newer commit of Seastar is usually easy, because Seastar
promises *source compatibility*: an application that compiled with an
older Seastar should compile with a newer one. It must be rebuilt, though:
an application can't be linked with a Seastar other than the one it was
compiled with. The promise covers only Seastar's public names, not those
in the namespace `seastar::internal`, which you will see in Seastar's
headers but should never use. An API that Seastar replaces keeps working
for a while, and your compiler warns that it's *deprecated*. Seastar also
supports only the last two C++ standards (today, C++23 and C++26) and the
two newest major versions of GCC and of Clang, so an application that must
build with an older compiler can't use a newer Seastar.
[Chapter](#ch:upgrading-seastar) explains the rest of what Seastar
promises, and what it doesn't, and how to upgrade.

## Building Seastar

Seastar depends on several libraries: Boost, fmt, c-ares, GnuTLS,
liburing, hwloc and others. On the common Linux distributions (Fedora,
Red Hat and its relatives, Debian and Ubuntu, and Arch), a script in
Seastar's source installs them all from the distribution's packages:

```
$ sudo ./install-dependencies.sh
```

Then *configure* the build, choosing a *mode*, and then build it:

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

### Build modes

The mode chooses how Seastar is compiled. The four main modes are:

| Mode | Optimized | Checks | Use it for |
|---|---|---|---|
| `release` | fully | assertions | production, and measuring performance |
| `dev` | partly | assertions | the edit, compile and test cycle |
| `debug` | no | everything, including sanitizers | finding bugs, and debugging with gdb |
| `sanitize` | for size | everything, including sanitizers | finding bugs in larger tests |

The debug modes are much slower, by a factor of a hundred or more, but they
find bugs that the other modes hide: they check every memory access, and
they deliberately run Seastar's tasks in an unpredictable order, to expose
code that depends on luck. [Chapter](#ch:testing) explains what they do.
You build and install each mode separately, and an application must be
built in the same mode as the Seastar it uses. This book uses `release`
unless it says otherwise.

### Choosing a compiler

Seastar builds with both GCC and Clang. By default, `configure.py` uses
`g++`; to use Clang, say so:

```
$ ./configure.py --mode=release --compiler=clang++ --prefix=$HOME/seastar-install
```

Either compiler works, but if you are starting a serious project, we
recommend Clang. Both large Seastar applications use it. ScyllaDB moved
from GCC to Clang in 2020, when Clang's support for coroutines was more
mature than GCC's, and since then has come to rely on other parts of the
LLVM toolchain too. Redpanda builds with a version of Clang that it pins
and downloads, so that all its developers use exactly the same compiler.
And Seastar's own automatic tests build the debug and sanitize modes
only with Clang, so those modes are better tested with it. GCC remains a
good choice, especially since it's the default compiler of most
distributions. All the code in this book was checked with both.

Whichever you choose, build your application with the same compiler as
Seastar. Mixing them is not supported, and doesn't even get far, because
some of the compiler options that Seastar passes on to your build are
specific to the compiler that built it.

`configure.py` also chooses the C++ standard. By default, it uses the
newest one your compiler supports, today C++26 with recent compilers.
Since this book promises only C++23, you can ask for that explicitly with
`--c++-standard=23`. Your application must then be compiled with C++23
or newer.

### When a dependency doesn't fit

Sometimes a library that your distribution provides is too old for
Seastar, or, more rarely, has a bug that Seastar can't live with. For
such cases, `configure.py` can download a known-good version of the
library, and build it as part of Seastar's build. This is called
*cooking* it. For example, when we built Seastar on Fedora 44 for this
book, `configure.py` refused the distribution's c-ares, a library for
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
all, Seastar's source also includes the description of a Docker
container with everything needed to build it; `doc/building-docker.md`
explains how to use it.

[Appendix](#app:building-seastar) covers building Seastar in more
detail: the rest of `configure.py`'s options, building with CMake directly, using
Seastar without installing it, and the pitfalls we met along the way.

## Building your own program

Once Seastar is installed, there are two ways to build a program that
uses it. [Chapter](#ch:tutorial-introduction) used the simplest one:
`pkg-config`, which tells the compiler where Seastar's headers are and
which libraries to link with. A single command builds a single-file
program:

```
$ export PKG_CONFIG_PATH=$HOME/seastar-install/lib64/pkgconfig
$ g++ -O2 hello.cc -o hello $(pkg-config --cflags --libs --static seastar)
```

(On some distributions, the directory is `lib` instead of `lib64`.)

The other way is CMake, and it's the better choice for anything bigger
than a single file. This is how Shoal is built, starting with its first
version below. Its whole build description, its `CMakeLists.txt` file,
is short:

```{.cmake include="shoal/v0/CMakeLists.txt" snippet="cmake"}
```

`find_package(Seastar)` finds the installed Seastar and the libraries it
depends on, and linking with `Seastar::seastar` adds everything needed to
compile and link with it: the include paths and the libraries. The other
settings matter too. Unless told otherwise, CMake compiles without any
optimization, which makes a Seastar program several times slower (we
measured Shoal's next version at a seventh of its speed), so we make the
default `RelWithDebInfo`: optimized, and with debugging information, like
Seastar's own `release` mode. ([Chapter](#ch:futures-and-promises)
explains why optimization matters so much for Seastar code, and
[chapter](#ch:deploying-and-tuning) which further options production
builds use.) Seastar's CMake files warn when the
application's C++ standard isn't the one Seastar was built with, so we set
C++23 as the default. The command line can still change both, with
`-DCMAKE_BUILD_TYPE` and `-DCMAKE_CXX_STANDARD`. And the range of versions
in `cmake_minimum_required()` lets newer versions of CMake use their newer
behavior, which they otherwise warn about when Seastar's files look for
Boost. To build Shoal in a directory `build`, tell CMake which compiler to
use and where to find Seastar:

```
$ cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=g++ \
    -DCMAKE_PREFIX_PATH="$HOME/seastar-install;$SEASTAR_SRC/build/release/_cooking/installed"
$ cmake --build build
```

Here `$SEASTAR_SRC` is where Seastar's source is. The second directory
in `CMAKE_PREFIX_PATH` is where the cooked libraries are; if you didn't
cook anything, you can leave it out. And if you install Seastar where
CMake looks by default, such as `/usr/local`, you can leave out
`CMAKE_PREFIX_PATH` altogether.

CMake has one more advantage when you cook libraries: the program it
builds remembers where the cooked libraries are, so it uses them when it
runs. A program built with the `pkg-config` command above doesn't, so at
run time it uses the distribution's version of the library, if there is
one, which may be exactly the version that Seastar refused. For
experiments, this rarely matters; for a real application, it's another
reason to use CMake.

## The C++ this book assumes

This book assumes C++23, the older of the two standards Seastar supports.
From C++20, it relies on coroutines, the subject of
[chapter](#ch:coroutines), on concepts, which Seastar uses to describe
what its templates accept, and on `std::source_location`, which tells a
function where it was called from. From C++23 it uses `std::print()` and
`std::println()`, which you met in [chapter](#ch:tutorial-introduction),
and *deducing this*, which solves one of the trickiest problems of
lambda coroutines, as [chapter](#ch:object-lifetime) explains. Nothing in
this book needs C++26.

When the book uses a part of C++ that many programmers haven't met, it
explains it in a short section called a *C++ corner*.
[Appendix](#app:cpp-corners) collects them all.

### Names

Everything Seastar provides is in the `seastar` namespace. Writing
`seastar::` before every name quickly becomes tiring, so Seastar
applications usually avoid it, in one of two ways. ScyllaDB, like
Seastar's own demos, writes `using namespace seastar;` and uses Seastar's
names unqualified. Redpanda defines a short alias, `namespace ss =
seastar;`, and writes `ss::future<>` and `ss::sleep()`; its code has tens
of thousands of these `ss::`.

This book follows ScyllaDB: its code uses Seastar's names without a
prefix. When the text first mentions a name, it gives the full name
(`seastar::sharded`), so you know where it comes from, and the index
lists the full names.

There is one catch. A few of Seastar's names are also names in the
standard library: `seastar::future` and `std::future`,
`seastar::promise` and `std::promise`, and `shared_ptr` and
`make_shared` in both. With `using namespace seastar` and `using
namespace std` together, the compiler can't tell which one you mean.
The simple rule is never to write `using namespace std` in a Seastar
program, and this book doesn't. A larger program, like ScyllaDB, puts
the `using` and a few clarifying declarations in a small header of its
own, which every source file includes; Shoal will do the same when it
grows beyond one file.

## Shoal, version 0

It's time to start Shoal.[^shoal-v0] As [chapter](#ch:case-for-seastar)
said, Shoal is a key-value store that speaks the Redis protocol, and by the
end of the book it stores its data on disk, on every core. First, a word
about its name.

[^shoal-v0]: The complete source of every version of Shoal is in the
    book's source, in its own directory. This chapter's version is in
    `shoal/v0/`.

> **About the name.** *Shoal* has one syllable, and rhymes with *goal* and
> *bowl*. A shoal is a group of fish that stay together. Biologists
> distinguish a shoal, whose fish keep together but each swim on their
> own, from a *school*, whose fish swim in coordination, all in the same
> direction. A shoal is a good picture of a sharded server: shards that
> work independently, with no shared state and no leader, yet together
> form one service. And it swims in the same sea as Seastar, whose name
> is both the name of a sea creature and a pun on *C\**, a common
> shorthand for Cassandra.

Version 0 doesn't store or serve anything yet. It reads its options,
starts on every shard, logs what it found, and exits. But every later
version starts the same way, so it's worth looking at closely. It begins
with a *logger*:

```{.cpp include="shoal/v0/main.cc" snippet="logger"}
```

A `seastar::logger` has a name, here "shoal", and methods for messages of
each *log level*: `error()`, `warn()`, `info()`, `debug()` and
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
`add_options()` declares Shoal's own command-line option, `--port`, with
its type, its default value and its help text. This uses the Boost
library's `program_options`, which `app_template` uses to parse the
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
coroutines, explained in [chapter](#ch:object-lifetime). Passing the
options to a separate coroutine function, by value, avoids it: a
coroutine keeps its own copy of its parameters for as long as it runs.

Build Shoal with CMake as shown above, and run it:

```
$ ./build/shoal -c2 -m1G
INFO  2026-11-06 16:25:37,664 [shard 0:main] shoal - Shoal starting on 2 shards, port 6379
INFO  2026-11-06 16:25:37,664 [shard 0:main] shoal - Shoal version 0 can't serve anything yet; exiting
```

Each log message shows its level, the time, the shard that logged it,
and the logger's name. To see the `debug` messages too, raise the log
level of the "shoal" logger, and let's also choose a different port:

```
$ ./build/shoal -c2 -m1G --port 6400 --logger-log-level shoal=debug
INFO  2026-11-06 16:19:33,428 [shard 0:main] shoal - Shoal starting on 2 shards, port 6400
DEBUG 2026-11-06 16:19:33,428 [shard 0:main] shoal - shard 0 is ready
DEBUG 2026-11-06 16:19:33,428 [shard 1:main] shoal - shard 1 is ready
INFO  2026-11-06 16:19:33,428 [shard 0:main] shoal - Shoal version 0 can't serve anything yet; exiting
```

`--logger-log-level` sets the level of individual loggers, and
`--default-log-level` sets the level of all the others. `--help-loggers`
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

Options can also be given in the `SEASTAR_OPTIONS` environment variable,
written exactly as on the command line, Seastar's options and the
program's own alike. If an option appears in both, the command line wins.
This is convenient for experiments, when you want to give the same options
to many runs, or to a program started by a script:

```
$ export SEASTAR_OPTIONS='-c2 -m1G'
$ ./build/shoal
INFO  2026-11-06 16:25:37,664 [shard 0:main] shoal - Shoal starting on 2 shards, port 6379
```

## Getting help, and helping

Seastar's documentation is in its source, in the `doc/` directory, and
its API reference, generated from the comments in its headers, is online
at <https://docs.seastar.io/>. The headers themselves are often the best
reference: most public functions have a comment explaining what they do.

Seastar is developed on GitHub. Questions and discussions about using it
go to the *Discussions* tab of its GitHub page, and bugs to its *Issues*.
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
   values. Which value does Shoal use in each case?

3. Run Shoal with `--logger-log-level shoal=trace`, and then with
   `--default-log-level debug`. Where do the extra messages come from in
   each case? Among the messages of the "seastar" logger, find the ones
   that say which CPU each shard runs on, and how much memory it got. Run
   Shoal with `-c2`, and then with `-c4`: which CPUs does Seastar choose?
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
