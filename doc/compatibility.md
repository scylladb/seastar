Compatibility
=============

As a library, Seastar aims to maintain backwards compatibility
in terms of the source (application code should continue to
build with newer versions of Seastar) and any binary protocols
that Seastar exposes (e.g. rpc).

Link compatibility is not maintained - you cannot link an
application built with one version of Seastar with another
version of Seastar.

Language standards
==================

Seastar will support the last two standards approved by the
ISO C++ committee. For example, after C++20 is released,
Seastar supports C++17 and C++20.  Similarly, when C++23 is released,
Seastar will support C++20 and C++23.

Some features may only be enabled for newer dialects.


Platforms
=========

Seastar supports Linux. There is no known minimum kernel version,
but very old kernels might not work. Performance can be significantly
better for newer kernels.

Filesystem implementation quality can have significant effect on
file I/O performance. XFS is known to be working, ext4 may work well
too. Test your filesystem and kernel versions to be sure.

Patches for new platforms (e.g, Windows) are welcome.


Compilers
=========

Seastar supports GCC and Clang. Ports to other compilers are
welcome.

The last two major releases of a compiler are supported (e.g.
GCC 13 and GCC 14). Patches to support older versions are welcome,
as long as they don't require onerous compromises.

Deprecation
===========

Occasionally, we discover that we took the wrong approach with
an API. In these cases we will offer a new API and tag the old
API with the [[deprecated]] attribute. The deprecated API will
be removed after a transition period (which can vary depending on
how central the deprecated API is).

Breaking changes
================

Rarely, we have to make breaking changes. We try to limit those,
but sometimes there is no choice.

To support a transition period for breaking changes, Seastar
offers the Seastar_API_LEVEL cmake variable (and corresponding
--api-level configure.py option). An API level selects different
versions of the API. For example.

   - Seastar_API_LEVEL=1 selects an old version of the
     server_socket::accept() API that returns a variadic
     future (which is deprecated)
   - Seastar_API_LEVEL=2 selects a new version of the
     server_socket::accept() API that returns a non-variadic
     future
   - Seastar_API_LEVEL=6 makes futures non-variadic
   - Seastar_API_LEVEL=7 unifies CPU scheduling groups and IO priority classes
     "while at it" file_impl API is forced to accept io_intent argument

Applications can use an old API_LEVEL during a transition
period, fix their code, and move to the new API_LEVEL.

Old API levels only live for a transition period, so if
you are using an API level below the latest, you should
upgrade quickly.

Note the application should not refer to the `api_vN`
sub-namespaces that Seastar defines as part of the API_LEVEL
mechanism; these are internal.

Internal namespace
==================

Identifiers in the `seastar::internal` namespace are not subject
to source level compatibility and are subject to change or removal
without notice. In addition the `api_vN` sub-namespaces are also
internal.

Accidentally exposed internal identifiers
=========================================

Some identifiers predate the internal namespace, and are only
exposed accidentally. These can also be removed or changed. Exposed
identifiers are documented using doxygen, but not all exposed
APIs are documented. In case of doubt, ask on the mailing list.


API Level History
=================

|Level|Introduced |Mandatory|Description                                   |
|:---:|:---------:|:-------:| -------------------------------------------- |
| 2   |  2019-07  | 2020-04 | Non-variadic futures in socket::accept()     |
| 3   |  2020-05  | 2023-03 | make_file_data_sink() closes file and returns a future<>  |
| 4   |  2020-06  | 2023-03 | Non-variadic futures in when_all_succeed()   |
| 5   |  2020-08  | 2023-03 | future::get() returns std::monostate() instead of void |
| 6   |  2020-09  | 2023-03 | future<T> instead of future<T...>            |
| 7   |  2023-05  | 2024-09 | unified CPU/IO scheduling groups             |


Note: The "mandatory" column indicates when backwards compatibility
support for the API preceding the new level was removed.

Implementation notes for API levels
===================================

API levels are implemented by defining internal sub-namespaces
for each API level: `seastar::api_v1`, `seatar::api_v2` etc. `#ifdef`s
are used to inline the user-selected API level namespace into the
main `seastar` namespace, making it visible.

Usually, the old API is implemented in terms of the new API to
avoid code duplication.

Here is an example about the transition from API_LEVEL 1 to 2. The
transition from 2 to 3 and similar is analogous.

Unconditionally:
 - the new API is defined in sub-namespace `api_v2`

If API_LEVEL is 2:
 - `api_v2` namespace is inlined into the `seastar` namespace

If API_LEVEL is 1:
 - the old API is defined in sub-namespace `api_v1`
 - `api_v1` is implemented in terms of `api_v2` to prevent code duplication
 - `api_v1` namespace is inlined into the `seastar` namespace

After a transition period:
 - everthing in `api_v1` is dropped
 - `api_v2` is removed, and its contents is placed in the parent namespace
