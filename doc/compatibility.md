Compatibility
=============

As a library, Seastar aims to maintain backward compatibility
in terms of the source (application code should continue to
build with newer versions of Seastar) and any binary protocols
that Seastar exposes (for example, RPC).

Link compatibility is not maintained - you cannot link an
application built with one version of Seastar with another
version of Seastar.

Language standards
==================

Seastar will support the last two standards approved by the
ISO C++ committee. Each time a new standard is approved, support
for the older of the two is retired. See
[Seastar's C++ standard](../README.md#cxx-standard)
in the README for the pair currently supported.

Some features may only be enabled for newer dialects.


Platforms
=========

Seastar supports Linux. There is no known minimum kernel version,
but very old kernels might not work. Performance can be significantly
better for newer kernels.

Filesystem implementation quality can have a significant effect on
file I/O performance. XFS is known to be working, ext4 may work well
too. Test your filesystem and kernel versions to be sure.

Patches for new platforms (for example, Windows) are welcome.


Compilers
=========

Seastar supports GCC and Clang. Ports to other compilers are
welcome.

The last two major releases of a compiler are supported (for example,
GCC 13 and GCC 14). Patches to support older versions are welcome,
as long as they don't require onerous compromises.

Deprecation
===========

Occasionally, we discover that we took the wrong approach with
an API. In these cases we will offer a new API and tag the old
API with the `[[deprecated]]` attribute. The deprecated API will
be removed after a transition period (which can vary depending on
how central the deprecated API is).

Breaking changes
================

Rarely, we have to make breaking changes. We try to limit those,
but sometimes there is no choice.

To support a transition period for breaking changes, Seastar
offers the `Seastar_API_LEVEL` CMake variable (and the corresponding
`--api-level` option to `configure.py`). An API level selects different
versions of the API. For example:

   - Seastar_API_LEVEL=1 selects an old version of the
     server_socket::accept() API that returns a variadic
     future (which is deprecated)
   - Seastar_API_LEVEL=2 selects a new version of the
     server_socket::accept() API that returns a non-variadic
     future
   - Seastar_API_LEVEL=6 makes futures non-variadic
   - Seastar_API_LEVEL=7 unifies CPU scheduling groups and IO priority classes
     "while at it" file_impl API is forced to accept io_intent argument
   - Seastar_API_LEVEL=8 changes json_return_type to hold a noncopyable function
     and become a move-only type
   - Seastar_API_LEVEL=9 defines the data_sink_impl::put(span<temporary_buffer>)
     as the new and only method to be implemented
   - Seastar_API_LEVEL=10 makes co_return and promise.set_value() semantics
     closer to those of plain C++ return regarding how they convert
     expressions used as their arguments to the declared type.


Applications can use an old API_LEVEL during a transition
period, fix their code, and move to the new API_LEVEL.

Old API levels only live for a transition period, so if
you are using an API level below the latest, you should
upgrade quickly.

Note that applications should not refer to the `api_vN`
sub-namespaces that Seastar defines as part of the API_LEVEL
mechanism; these are internal.

Internal namespace
==================

Identifiers in the `seastar::internal` namespace are not subject
to source-level compatibility and are subject to change or removal
without notice. In addition, the `api_vN` sub-namespaces are also
internal.

Accidentally exposed internal identifiers
=========================================

Some identifiers predate the internal namespace, and are only
exposed accidentally. These can also be removed or changed. Exposed
identifiers are documented using Doxygen, but not all exposed
APIs are documented. In case of doubt, ask on the mailing list.


API Level History
=================

|Level|Introduced |Mandatory|Description                                   |
|:---:|:---------:|:-------:| -------------------------------------------- |
|  2  |  2019-07  | 2020-04 | Non-variadic futures in socket::accept()     |
|  3  |  2020-05  | 2023-03 | make_file_data_sink() closes file and returns a future<>  |
|  4  |  2020-06  | 2023-03 | Non-variadic futures in when_all_succeed()   |
|  5  |  2020-08  | 2023-03 | future::get() returns std::monostate() instead of void |
|  6  |  2020-09  | 2023-03 | future<T> instead of future<T...>            |
|  7  |  2023-05  | 2024-09 | unified CPU/IO scheduling groups             |
|  8  |  2025-08  |         | noncopyable function in json_return_type     |
|  9  |  2025-08  |         | data_sink_impl new API                       |
| 10  |  2026-04  |         | co_return and set_value strict type semantics |

Note: The "Mandatory" column indicates when backward-compatibility
support for the API preceding the new level was removed.

Implementation notes for API levels
===================================

API levels are implemented by defining internal sub-namespaces
for each API level: `seastar::api_v1`, `seastar::api_v2`, etc. `#ifdef`s
are used to inline the user-selected API level namespace into the
main `seastar` namespace, making it visible.

Usually, the old API is implemented in terms of the new API to
avoid code duplication.

Here is an example of the transition from `API_LEVEL` 1 to 2. The
transition from 2 to 3, and similar transitions, are analogous.

Unconditionally:
 - the new API is defined in sub-namespace `api_v2`

If API_LEVEL is 2:
 - `api_v2` namespace is inlined into the `seastar` namespace

If API_LEVEL is 1:
 - the old API is defined in sub-namespace `api_v1`
 - `api_v1` is implemented in terms of `api_v2` to prevent code duplication
 - `api_v1` namespace is inlined into the `seastar` namespace

After a transition period:
 - everything in `api_v1` is dropped
 - `api_v2` is removed, and its contents are placed in the parent namespace
