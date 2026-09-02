Seastar
=======

[![Test](https://github.com/scylladb/seastar/actions/workflows/tests.yaml/badge.svg)](https://github.com/scylladb/seastar/actions/workflows/tests.yaml)
[![Version](https://img.shields.io/github/tag/scylladb/seastar.svg?label=version&colorB=green)](https://github.com/scylladb/seastar/releases)
[![License: Apache2](https://img.shields.io/github/license/scylladb/seastar.svg)](https://github.com/scylladb/seastar/blob/master/LICENSE)
[![n00b issues](https://img.shields.io/github/issues/scylladb/seastar/n00b.svg?colorB=green)](https://github.com/scylladb/seastar/labels/n00b)

Introduction
------------

Seastar is an event-driven framework that allows you to write non-blocking,
asynchronous code in a relatively straightforward manner (once understood).
It is based on [futures](https://en.wikipedia.org/wiki/Futures_and_promises).

Building Seastar
--------------------

For more details and alternative workflows, read [HACKING.md](./HACKING.md).

Assuming that you would like to use system packages (RPMs or DEBs) for Seastar's dependencies, first install them:

```
$ sudo ./install-dependencies.sh
```

Then configure (in "release" mode):

```
$ ./configure.py --mode=release
```
Then compile:

```
$ ninja -C build/release
```

If compilation fails with an error such as `g++: internal compiler error: Killed (program cc1plus)`,
try giving GCC more memory. You can limit the number of parallel jobs with `-j1` and/or allocate at least 4 GiB of RAM to your
machine.

If you're missing a dependency of Seastar, then it is possible to have the configuration process fetch a version of the dependency locally for development.

For example, to fetch `fmt` locally, configure Seastar like this:

```
$ ./configure.py --mode=dev --cook fmt
```

`--cook` can be repeated many times for selecting multiple dependencies.


Build modes
----------------------------------------------------------------------------

The `configure.py` script is a wrapper around CMake. The `--mode` argument
maps to `CMAKE_BUILD_TYPE` and supports the following modes:

|          | CMake mode          | Debug info | Optimi&shy;zations | Sanitizers   | Allocator | Checks   | Use for                                |
| -------- | ------------------- | ---------- | ------------------ |------------- | --------- | -------- | -------------------------------------- |
| debug    | `Debug`             | Yes        | `-O0`              | ASAN, UBSAN  | System    | All      | gdb                                    |
| release  | `RelWithDebInfo`    | Yes        | `-O3`              | None         | Seastar   | Asserts  | production                             |
| dev      | `Dev` (Custom)      | No         | `-O1`              | None         | Seastar   | Asserts  | build and test cycle                   |
| sanitize | `Sanitize` (Custom) | Yes        | `-Os`              | ASAN, UBSAN  | System    | All      | second level of tests, track down bugs |

Note that Seastar is more sensitive to allocators and optimizations than
usual. As a rough rule of thumb, `release` is twice as fast as `dev`, 150
times as fast as `sanitize`, and 300 times as fast as `debug`.

Using Seastar from its build directory (without installation)
----------------------------------------------------------------------------

It's possible to consume Seastar directly from its build directory with CMake or `pkg-config`.

We'll assume that the Seastar repository is located in a directory at `$seastar_dir`.


Via `pkg-config`:

```
$ g++ my_app.cc $(pkg-config --libs --cflags --static $seastar_dir/build/release/seastar.pc) -o my_app
```

and with CMake using the `Seastar` package:


`CMakeLists.txt` for `my_app`:

```
set (CMAKE_CXX_STANDARD 23)

find_package (Seastar REQUIRED)

add_executable (my_app
  my_app.cc)

target_link_libraries (my_app
  Seastar::seastar)
```

```
$ mkdir $my_app_dir/build
$ cd $my_app_dir/build
$ cmake -DCMAKE_PREFIX_PATH="$seastar_dir/build/release;$seastar_dir/build/release/_cooking/installed" -DCMAKE_MODULE_PATH=$seastar_dir/cmake $my_app_dir
```

The `CMAKE_PREFIX_PATH` values ensure that CMake can locate Seastar and its compiled submodules. The `CMAKE_MODULE_PATH` value ensures that CMake can use Seastar's CMake scripts to locate its dependencies.

Using an installed Seastar
--------------------------------

You can also consume Seastar after it has been installed to the filesystem.

**Important:**

- Seastar works with a customized version of DPDK, so by default builds and installs the DPDK submodule to `$build_dir/_cooking/installed`

First, configure the installation path:

```
$ ./configure.py --mode=release --prefix=/usr/local
```

Then run the `install` target:

```
$ ninja -C build/release install
```

Then consume it from `pkg-config`:

```
$ g++ my_app.cc $(pkg-config --libs --cflags --static seastar) -o my_app
```

or consume it with the same `CMakeLists.txt` as before but with a simpler CMake invocation:

```
$ cmake ..
```

(If Seastar has not been installed to a "standard" location like `/usr` or `/usr/local`, then you can invoke CMake with `-DCMAKE_PREFIX_PATH=$my_install_root`.)

There are also instructions for building on any host that supports [Docker](doc/building-docker.md).

Use of [DPDK](https://www.dpdk.org/) is [optional](doc/building-dpdk.md).

#### <a id="cxx-standard"></a>Seastar's C++ standard: C++23 or C++26

Seastar supports both C++23 and C++26. The build defaults to the latest
standard supported by your compiler, but can be explicitly selected with
the `--c++-standard` configure option, e.g., `--c++-standard=23`,
or, if using CMake directly, by setting the `CMAKE_CXX_STANDARD` CMake
variable.

See the [compatibility statement](doc/compatibility.md) for more information.

Getting started
---------------

There is a [mini tutorial](doc/mini-tutorial.md) and a [more comprehensive one](doc/tutorial.md).

The documentation is available on the [web](https://docs.seastar.io/master/index.html).


Resources
---------

* Seastar Development Mailing List: Discuss challenges, propose improvements, send
  code contributions (patches), and get help from experienced developers.
  Subscribe or browse archives: [here](https://groups.google.com/g/seastar-dev)
  (or email seastar-dev@googlegroups.com).
* GitHub Discussions: For more casual conversations and quick questions, consider
  using the Seastar project's [discussions on GitHub](https://github.com/scylladb/seastar/discussions).
* Issue Tracker: File bug reports on the project's [issue tracker](https://github.com/scylladb/seastar/issues).

Learn more about Seastar on the main [project website](https://seastar.io/).

The Native TCP/IP Stack
-----------------------

Seastar comes with its own [userspace TCP/IP stack](doc/native-stack.md) for better performance.

Recommended hardware configuration for Seastar
----------------------------------------------

* CPUs - As many as you need. Seastar is well suited to multicore and NUMA systems.
* NICs - As fast as possible; we recommend 10G or 40G cards. It is possible to use
       1G cards too, but you may be limited by their capacity.
       In addition, the more hardware queues per CPU, the better for Seastar.
       Otherwise we have to emulate that in software.
* Disks - Fast SSDs with a high number of IOPS.
* Client machines - Usually a single client machine cannot fully load our servers.
       Both memaslap (memcached) and wrk (httpd) may be unable to overload their matching
       server counterparts. We recommend running clients on machines other than the servers
       and using several of them.

Projects using Seastar
----------------------------------------------

* [cpv-cql-driver](https://github.com/cpv-project/cpv-cql-driver): C++ driver for Cassandra/Scylla based on the Seastar framework
* [cpv-framework](https://github.com/cpv-project/cpv-framework): A web framework written in C++ based on the Seastar framework
* [Redpanda](https://www.redpanda.com/): A Kafka-compatible streaming data platform for mission-critical systems
* [Scylla](https://github.com/scylladb/scylla): A fast and reliable NoSQL data store compatible with Cassandra and DynamoDB
* [smf](https://github.com/smfrpc/smf): The fastest RPC in the West
* [Ceph - Crimson](https://github.com/ceph/ceph): Next-generation OSD (Object Storage Daemon) implementation based on the Seastar framework
