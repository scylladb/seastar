# Building Seastar {#app:building-seastar}

[Chapter](#ch:getting-started) showed the usual way to build Seastar:
install its dependencies with `install-dependencies.sh`, configure it
with `configure.py`, build it with `ninja` and install it. This appendix
covers the rest: what `configure.py` really does, and its other options;
building Seastar with CMake directly; using Seastar without installing
it; building with DPDK or in a container; Seastar as a C++ module; and
the pitfalls we met while building Seastar for this book.

## What `configure.py` does

`configure.py` is a thin wrapper around CMake. For each build mode, it
runs CMake once, in a build directory of its own, `build/MODE`, and
translates its own options into CMake variables. The mode becomes
CMake's *build type*:

| Mode | CMake build type | Library |
|---|---|---|
| `release` | `RelWithDebInfo` | static, `libseastar.a` |
| `dev` | `Dev` | shared, `libseastar.so` |
| `debug` | `Debug` | shared, `libseastar.so` |
| `sanitize` | `Sanitize` | static, `libseastar.a` |
| `fuzz` | `Fuzz` | static, `libseastar.a` |

`Dev`, `Sanitize` and `Fuzz` are build types that Seastar's CMake files
define; the other two are CMake's own. The `fuzz` mode, which
[chapter](#ch:getting-started)'s table left out, builds Seastar for
*fuzz testing* with LLVM's libFuzzer, which feeds a program random inputs
to look for crashes. It needs Clang, and is used mainly by Seastar's own
fuzz tests.

Two details of this table matter in practice:

- **Without `--mode`, `configure.py` configures all five modes,** and
  `ninja -C build/release` then builds only one of them. But each mode
  is configured, and its dependencies cooked, separately, so it's
  faster to name the mode you want.
- **The `dev` and `debug` modes build Seastar as a shared library.** A
  program linked with it must find `libseastar.so` when it starts. A
  program built with CMake gets the library's directory built into it (an
  *rpath*), but one built with `pkg-config` doesn't, and fails to start
  with "libseastar.so: cannot open shared object file" until
  `LD_LIBRARY_PATH` names the directory.

### The other options

[Chapter](#ch:getting-started) used `--mode`, `--prefix`, `--compiler`,
`--c++-standard`, `--cook` and `--without-tests`, `--without-apps` and
`--without-demos`. `./configure.py --help` lists the rest. These are the
ones worth knowing:

| Option | Effect |
|---|---|
| `--c-compiler CC` | The C compiler, for the few dependencies written in C, such as DPDK. Give it with `--compiler` (for example `--compiler clang++ --c-compiler clang`). |
| `--cflags`, `--ldflags` | Extra flags for compiling and linking Seastar. |
| `--optflags` | Extra flags for the `release` mode only, such as `-march=native`. |
| `--compiler-cache` | `auto` (the default) uses sccache, or else ccache, when installed; `none` disables it. A cache makes rebuilding Seastar after switching branches much faster. |
| `--build-root DIR` | Build in `DIR/MODE` instead of `build/MODE`, so that several configurations, say with GCC and with Clang, can live in one source tree. |
| `--compile-commands-json` | Write `compile_commands.json`, which editors that use clangd need in order to understand the code. |
| `--split-dwarf` | Keep debugging information out of the object files, which makes linking faster. |
| `--api-level N` | Which version of Seastar's API to compile, for applications that haven't caught up with a change yet. [Chapter](#ch:upgrading-seastar) explains API levels. |
| `--scheduling-groups-count N` | The maximum number of scheduling groups ([chapter](#ch:scheduling-groups)), 16 by default. |
| `--enable-io_uring`, `--disable-io_uring` | Whether to include the `io_uring` reactor backend ([chapter](#ch:how-seastar-runs-your-code)). By default, it's included when `liburing` is installed. |
| `--enable-dpdk` | Include DPDK; see below. |
| `--disable-gnutls`, `--disable-openssl` | Which libraries for TLS and cryptography to build in. By default, Seastar builds in each of GnuTLS and OpenSSL that is installed, and needs at least one. |
| `--disable-hwloc` | Don't use hwloc, the library that tells Seastar which cores and memory belong to which processor socket. |
| `--disable-lttng` | Don't include the LTTng tracepoints for tracing I/O. |
| `--enable-task-backtrace` | Record a backtrace wherever code waits, so that the backtraces Seastar prints, for example of an exception, also show the code waiting for the current task. Slower; for debugging. |
| `--enable-alloc-failure-injector` | Let tests make memory allocations fail on purpose, to test the code that handles failure ([chapter](#ch:memory)). It's on by default in `dev` mode. |
| `--enable-debug-shared-ptr` | Check that a `shared_ptr` or `lw_shared_ptr` is only used on the shard that created it ([chapter](#ch:object-lifetime)). It's on by default in `debug` and `sanitize` modes. |
| `--heap-profiling` | Support sampling where memory is allocated ([chapter](#ch:memory)). |
| `--enable-unused-result-error` | Make ignoring a `[[nodiscard]]` result, such as a future, an error rather than a warning. |
| `--enable-cxx-modules` | Also build Seastar as a C++ module; see below. |

## Building with CMake directly

`configure.py` is a convenience: anything it does, CMake can do directly.
That's useful when an IDE runs CMake itself, or when Seastar is built as
part of a larger CMake project. The mode becomes `CMAKE_BUILD_TYPE`, and
the other options become CMake variables, most of them named `Seastar_`
followed by the option's name: `--enable-dpdk` is `-DSeastar_DPDK=ON`,
`--without-tests` is `-DSeastar_EXCLUDE_TESTS_FROM_ALL=ON`, and so on.
For example, this configures a release build with Clang and C++23, run
in Seastar's source directory:

```
$ cmake -S . -B build/rel -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_CXX_STANDARD=23 \
      -DSeastar_API_LEVEL=10 -DCMAKE_INSTALL_PREFIX=$HOME/seastar-install
$ ninja -C build/rel
```

Two of CMake's defaults differ from `configure.py`'s:

- **The API level.** `configure.py` asks for the newest API level, 10
  today, but Seastar's CMake files default to the previous one, 9. Give
  `-DSeastar_API_LEVEL=10` to get the same Seastar as with
  `configure.py`.
- **Shared libraries.** `configure.py` builds the `dev` and `debug` modes
  as shared libraries by setting `BUILD_SHARED_LIBS`; CMake builds a
  static library unless you do the same.

CMake can't cook dependencies by itself. Either install them all from
your distribution, or build the missing ones yourself and add their
directory to `CMAKE_PREFIX_PATH`, or use `cooking.sh`, the script that
`configure.py` uses for `--cook`. It builds the dependencies that `-i`
names, in the build directory that `-d` names, and then runs CMake with
them and with the options after `--`:

```
$ ./cooking.sh -i c-ares -d build/rel -t RelWithDebInfo -- \
      -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_CXX_STANDARD=23 -DSeastar_API_LEVEL=10
$ ninja -C build/rel
```

One more thing to know about CMake: it remembers the libraries it found,
in the build directory's `CMakeCache.txt`. If it found the wrong version
of a library, running `cmake` again with a different
`CMAKE_PREFIX_PATH` doesn't make it look again. Start over in a fresh
build directory.

## Using Seastar without installing it

An application doesn't have to use an installed Seastar. It can use
Seastar straight from Seastar's build directory, or build Seastar as
part of its own build.

### From Seastar's build directory

The build directory has a `seastar.pc` of its own, and a CMake package
file, so both ways of building a program from
[chapter](#ch:getting-started) work with it. Here `$SEASTAR` is the
directory of Seastar's source. With `pkg-config`, name the file:

```
$ g++ -O2 hello.cc -o hello \
      $(pkg-config --cflags --libs --static $SEASTAR/build/release/seastar.pc)
```

With CMake, tell `find_package()` where to look: in the build directory
and in the cooked dependencies, as for an installed Seastar, and also in
Seastar's own CMake scripts, which find Seastar's other dependencies (an
installed Seastar has copies of them, but the build directory doesn't):

```
$ cmake -S . -B build -G Ninja \
      -DCMAKE_PREFIX_PATH="$SEASTAR/build/release;$SEASTAR/build/release/_cooking/installed" \
      -DCMAKE_MODULE_PATH=$SEASTAR/cmake
```

This is handy while you change Seastar itself: rebuild it, and rebuild
your program, with no installing in between.

### As part of your own build

An application that keeps Seastar as a git submodule, as
[chapter](#ch:getting-started) suggests, can make Seastar part of its own
CMake build, with `add_subdirectory()`. Then a single `cmake` and a
single build compile both, with the same compiler and flags:

```cmake
cmake_minimum_required(VERSION 3.13...3.30)
project(shoal LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23 CACHE STRING "The C++ standard to use")
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "The build type" FORCE)
endif()

# Seastar, from the git submodule in seastar/, built as part of this project.
set(Seastar_API_LEVEL 10 CACHE STRING "Seastar's API level")
add_subdirectory(seastar)

add_executable(shoal main.cc)
target_link_libraries(shoal PRIVATE Seastar::seastar)
```

When Seastar is built this way, as part of another project, it doesn't
build its tests, demos, applications or documentation unless asked to,
and doesn't install itself. Set any other `Seastar_` options before
`add_subdirectory()`, as this file sets the API level. Seastar's
dependencies must be available as when building Seastar alone: for
example, on Fedora 44, give `cmake` a cooked c-ares's directory in
`CMAKE_PREFIX_PATH`. ScyllaDB's CMake build includes Seastar this way.

## Pitfalls

Building Seastar for this book, on Fedora 44, taught us a few things
that aren't obvious:

- **An installation with cooked libraries isn't self-contained.** The
  libraries that `--cook` built stay in Seastar's build directory, under
  `build/MODE/_cooking/installed`; installing Seastar doesn't copy them,
  and the installed `seastar.pc` refers to them where they are. So don't
  delete Seastar's build directory, and give CMake both directories in
  `CMAKE_PREFIX_PATH`, as [chapter](#ch:getting-started) did.
- **A `pkg-config` build may run with the wrong copy of a cooked
  library.** CMake builds a program with the cooked libraries' directory
  built into it, but `pkg-config` doesn't. So a program built with
  `pkg-config` links with the cooked `libcares.so`, but at run time loads
  the distribution's, the very version that `configure.py` rejected,
  unless `LD_LIBRARY_PATH` says otherwise.
- **The flags are the compiler's.** `pkg-config --cflags seastar`
  includes options that only the compiler that built Seastar accepts.
  Building a program with the other compiler fails.
- **Memory.** Seastar's source files are heavy with templates, and a
  single compiler process can take about a gigabyte of memory. `ninja` runs as many in
  parallel as the machine has cores, plus two; on a machine with many
  cores and not much memory, give it a lower `-j`.

## Building with DPDK

[Chapter](#ch:networking) explains Seastar's own TCP stack, which can
drive a network card directly through DPDK, bypassing the kernel.
Seastar carries its own copy of DPDK, as a git submodule in `dpdk/`.
To build with it, fetch the submodule, and enable DPDK:

```
$ git submodule update --init dpdk
$ ./configure.py --mode=release --enable-dpdk
$ ninja -C build/release
```

`configure.py` builds DPDK as it cooks dependencies, for the processor
that `--dpdk-machine` names, by default the one you are building on, and
then `ninja` builds Seastar with it. Every program built with this Seastar
can use DPDK: its `--help-seastar` lists `--dpdk-pmd` and other DPDK
options. But it uses DPDK only when it runs with `--dpdk-pmd`, and with
the native network stack, and that also needs a network card that DPDK
supports, set up for it, as [chapter](#ch:networking) explains. (We built
Seastar with DPDK, and ran programs built with it, but our laptop has no
such network card, so we couldn't run one with DPDK.) Seastar's
`README-DPDK.md` explains how to use a DPDK of your own instead.

## Building in a container

Seastar's source includes a Dockerfile, `docker/dev/Dockerfile`, which
describes a container with everything needed to build Seastar: an Ubuntu
system, with both compilers and all of Seastar's dependencies. Build the
container image once, in Seastar's source directory, and then run
`configure.py` and `ninja` in a container, with the source directory
mounted inside it, and as your own user, so that the files it builds
belong to you:

```
$ docker build -t seastar-dev -f docker/dev/Dockerfile .
$ docker run --rm -v $PWD:/seastar -w /seastar -u $(id -u):$(id -g) seastar-dev \
      ./configure.py --mode=release
$ docker run --rm -v $PWD:/seastar -w /seastar -u $(id -u):$(id -g) seastar-dev \
      ninja -C build/release
```

When we tried this, building the image took about ten minutes, mostly
downloading, and the container's libraries were new enough that nothing
had to be cooked. The Seastar built this way is linked with the
container's libraries, so build your programs in the container too.
`doc/building-docker.md` shows a shell function that shortens these
commands. On a system with SELinux, such as Fedora, a container can't
read a mounted directory unless you add `:z` to the `-v` option, which
relabels the directory for containers, or run the container with
`--security-opt label=disable`.

## Seastar as a C++ module

C++20 added *modules*, a replacement for `#include`. Instead of
compiling all of Seastar's headers again in every source file, the
compiler compiles them once into a *module*, and a source file that says
`import seastar;` loads the result. Since Seastar's headers are large,
this could make compiling programs much faster.

Seastar can be built as a module, with `--enable-cxx-modules` (or
`-DSeastar_MODULE=ON` with CMake, which needs CMake 3.26 or later and
C++20 or later). But the support is experimental: it is off by default,
Seastar's own automatic tests build only one small demonstration program
with it, and only with Clang, and we know of no application that uses
it. Every program in this book uses `#include`. To find out whether
modules are worth trying, we built Seastar as a module (Seastar's commit
6a752c502, in November 2026), and built Shoal version 1 with
`import seastar;` instead of its `#include`s.

**With Clang 22, it works,** with some adjustments:

- **Include what Seastar's module doesn't export.** A program that
  imports Seastar still needs some standard headers itself: `<coroutine>`
  to define coroutines, and `<functional>` to pass a lambda to
  `app_template::run()`. Include them, and the program's other headers,
  before `import seastar;`.
- **With CMake,** you need CMake 3.28 or later, the first version that
  can use a module from an installed package. CMake compiles Seastar's
  module once more, in your program's build directory, with the options
  that Seastar's package gives it.
- **With `pkg-config`,** use the compiled module that installing Seastar
  puts in `lib64/cxx/bmi/seastar.pcm`, by adding
  `-fmodule-file=seastar=PATH/seastar.pcm`. Clang refuses to use it
  unless the program is compiled with exactly the options it was compiled
  with: with `-std=gnu++23` (not `c++23`), and without `-pthread`, which
  `pkg-config --cflags` gives but is needed only for linking.

The result passed all of Shoal's tests, and compiling each of Shoal's
files took 40% to 50% less time than with `#include`, and 10% to 25%
less memory. But with CMake, the module itself must first be compiled in
each program's build directory, which took 7.5 seconds and almost a
gigabyte of memory. For Shoal's three files, that cost more than modules
saved: a clean build took 15 seconds instead of 7. A program with dozens
or hundreds of source files, which is where build times hurt, would come
out well ahead.

**With GCC 16, it doesn't work yet.** Seastar itself builds as a module,
but a program that imports it doesn't compile: GCC doesn't find, through
the module, how a coroutine that returns a Seastar `future` works. We
reproduced that last problem in a tiny program without Seastar, which
Clang compiles and GCC doesn't.[^gcc-modules]

> **Draft note, for the editors:** this section assumes that two Seastar
> pull requests are merged: <https://github.com/scylladb/seastar/pull/3810>,
> without which a CMake program that uses Seastar's module fails with
> Clang (add `-DCMAKE_CXX_FLAGS=-Wno-error=unused-command-line-argument`)
> and Seastar's module doesn't build with GCC at all, and
> <https://github.com/scylladb/seastar/pull/3811>, without which GCC
> rejects eleven places in Seastar's headers. Revisit this section when
> they are merged, or rejected.

[^gcc-modules]: We found no GCC bug report about exactly this. The
    closest, <https://gcc.gnu.org/bugzilla/show_bug.cgi?id=122609>, is
    about GCC not finding standard library names, such as
    `std::coroutine_traits`, that code needs where it is used, and says
    that a complete fix is planned for GCC 17.

So modules are a promising way to build large Seastar programs faster,
but not yet a practical one. If build times matter to you, they are
worth trying with Clang; check the state of Seastar's support first,
since it may have improved since we wrote this.
