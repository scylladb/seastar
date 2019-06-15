Seastar
=======

[![CircleCI](https://circleci.com/gh/scylladb/seastar.svg?style=svg)](https://circleci.com/gh/scylladb/seastar)
[![Version](https://img.shields.io/github/tag/scylladb/seastar.svg?label=version&colorB=green)](https://github.com/scylladb/seastar/releases)
[![License: Apache2](https://img.shields.io/github/license/scylladb/seastar.svg)](https://github.com/scylladb/seastar/blob/master/LICENSE)
[![n00b issues](https://img.shields.io/github/issues/scylladb/seastar/n00b.svg?colorB=green)](https://github.com/scylladb/seastar/labels/n00b)

Introduction
------------

SeaStar is an event-driven framework allowing you to write non-blocking,
asynchronous code in a relatively straightforward manner (once understood).
It is based on [futures](http://en.wikipedia.org/wiki/Futures_and_promises).

Building Seastar
--------------------

For more details and alternative work-flows, read [HACKING.md](./HACKING.md).

Assuming that you would like to use system packages (RPMs or DEBs) for Seastar's dependencies, first install them:

```
$ sudo ./install-dependencies.sh
```

then configure (in "release" mode):

```
$ ./configure.py --mode=release
```
then compile:

```
$ ninja -C build/release
```

If you're missing a dependency of Seastar, then it is possible to have the configuration process fetch a version of the dependency locally for development.

For example, to fetch `fmt` locally, configure Seastar like this:

```
$ ./configure.py --mode=dev --cook fmt
```

`--cook` can be repeated many times for selecting multiple dependencies.


Build modes
----------------------------------------------------------------------------

The configure.py script is a wrapper around cmake. The --mode argument
maps to CMAKE_BUILD_TYPE, and supports the following modes

          | CMake mode        | Debug info | Optimizations  | Sanitizers   | Allocator | Checks   | Use for                                |
 ---------+-------------------+------------+----------------+--------------+-----------+----------+----------------------------------------|
 debug    | Debug             | Yes        | -O0            | ASAN, UBSAN  | System    | All      | gdb                                    |
 release  | RelWithDebInfo    | Yes        | -O3            | None         | Seastar   | Asserts  | production                             |
 dev      | Dev (Custom)      | No         | -O1            | None         | Seastar   | Asserts  | build and test cycle                   |
 sanitize | Sanitize (Custom) | Yes        | -Os            | ASAN, UBSAN  | System    | All      | second level of tests, track down bugs |

Note that seastar is more sensitive to allocators and optimizations than
usual. A quick rule of the thumb of the relative performances is that
release is 2 times faster than dev, 150 times faster than sanitize and
300 times faster than debug.

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

The `CMAKE_PREFIX_PATH` values ensure that CMake can locate Seastar and its compiled submodules. The `CMAKE_MODULE_PATH` value ensures that CMake can uses Seastar's CMake scripts for locating its dependencies.

Using an installed Seastar
--------------------------------

You can also consume Seastar after it has been installed to the file-system.

**Important:**

- Seastar works with a customized version of DPDK, so by default builds and installs the DPDK submodule to `$build_dir/_cooking/installed`

First, configure the installation path:

```
$ ./configure.py --mode=release --prefix=/usr/local
```

then run the `install` target:

```
$ ninja -C build/release install
```

then consume it from `pkg-config`:

```
$ g++ my_app.cc $(pkg-config --libs --cflags --static seastar) -o my_app
```

or consume it with the same `CMakeLists.txt` as before but with a simpler CMake invocation:

```
$ cmake ..
```

(If Seastar has not been installed to a "standard" location like `/usr` or `/usr/local`, then you can invoke CMake with `-DCMAKE_PREFIX_PATH=$my_install_root`.)

There are also instructions for building on any host that supports [Docker](doc/building-docker.md).

Use of the [DPDK](http://dpdk.org) is [optional](doc/building-dpdk.md).

#### Seastar's C++ dialect: C++14 or C++17

Seastar supports both C++14, and C++17. Some newer features and optimizations
may only be available to C++17, so users are encouraged to use C++17 when
possible. By default Seastar builds with the C++17 dialect, but a C++14
compilation can be requested with a `./configure.py --c++-dialect=gnu++14`
or, if using CMake directly, by setting on the `Seastar_CXX_DIALECT` CMake
variable to `"gnu++14"`.

However, by default Seastar uses C++14-compatible types such as
`std::experimental::optional<>`, `boost::variant` and `std::experimental::string_view`, both internally and in its public
API, thus forcing them on C++17 projects. To fix this, Seastar respects the value of the preprocessor variable
`SEASTAR_USE_STD_OPTIONAL_VARIANT_STRINGVIEW`, which changes those types to their `stdlib` incarnation, and allows
seemless use of C++17. Usage of this option requires an updated compiler, such
as GCC 8.1.1-5 on Fedora.

Getting started
---------------

There is a [mini tutorial](doc/mini-tutorial.md) and a [more comprehensive one](doc/tutorial.md).

The documentation is available on the [web](http://docs.seastar.io/master/index.html).


Resources
---------
Ask questions and post patches on the development mailing list. Subscription
information and archives are available [here](https://groups.google.com/forum/#!forum/seastar-dev),
or just send an email to seastar-dev@googlegroups.com.

Information can be found on the main [project website](http://seastar.io).

File bug reports on the project [issue tracker](https://github.com/scylladb/seastar/issues).

The Native TCP/IP Stack
-----------------------

Seastar comes with its own [userspace TCP/IP stack](doc/native-stack.md) for better performance.

Recommended hardware configuration for SeaStar
----------------------------------------------

* CPUs - As much as you need. SeaStar is highly friendly for multi-core and NUMA
* NICs - As fast as possible, we recommend 10G or 40G cards. It's possible to use
       1G too but you may be limited by their capacity.
       In addition, the more hardware queue per cpu the better for SeaStar.
       Otherwise we have to emulate that in software.
* Disks - Fast SSDs with high number of IOPS.
* Client machines - Usually a single client machine can't load our servers.
       Both memaslap (memcached) and WRK (httpd) cannot over load their matching
       server counter parts. We recommend running the client on different machine
       than the servers and use several of them.
