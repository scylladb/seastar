Seastar
=======

[![Travis Build Status](https://travis-ci.org/scylladb/seastar.svg?branch=master)](https://travis-ci.org/scylladb/seastar)
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

See instructions for [Fedora](doc/building-fedora.md), [CentOS](doc/building-centos.md) and [Ubuntu](doc/building-ubuntu.md).

There are also instructions for building on any host that supports [Docker](doc/building-docker.md).

Use of the [DPDK](http://dpdk.org) is [optional](doc/building-dpdk.md).

#### Using C++17

Seastar can be built with the C++17 dialect by supporting compilers, conditional
on the `--c++-dialect` option being set to `gnu++17`.

However, by default Seastar uses C++14-compatible types such as
`std::experimental::optional<>` or `boost::variant`, both internally and in its public
API, thus forcing them on C++17 projects. To fix this, Seastar provides the `--use-std-optional-variant-stringview 0|1`
option, which changes those types to their `stdlib` incarnation, and allows
seemless use of C++17. Usage of this option requires an updated compiler, such
as GCC 8.1.1-5 on Fedora.

Getting started
---------------

There is a [mini tutorial](doc/mini-tutorial.md) and a [more comprehensive one](doc/tutorial.md).

The documentation is available on the [web](http://docs.seastar-project.org/).


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
