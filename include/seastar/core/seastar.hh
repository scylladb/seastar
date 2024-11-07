/*
 * This file is open source software, licensed to you under the terms
 * of the Apache License, Version 2.0 (the "License").  See the NOTICE file
 * distributed with this work for additional information regarding copyright
 * ownership.  You may not use this file except in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

/*
 * Copyright (C) 2015 Cloudius Systems, Ltd.
 */

#pragma once

/// \mainpage
///
/// Seastar is a high performance C++ application framework for high
/// concurrency server applications.
///
/// A good place to start is the [Tutorial](tutorial.html) or [Multi-page version](split/).
///
/// Please see:
///   - \ref future-module Documentation on futures and promises, which are
///          the seastar building blocks.
///   - \ref future-util Utililty functions for working with futures
///   - \ref memory-module Memory management
///   - \ref networking-module TCP/IP networking
///   - \ref fileio-module File Input/Output
///   - \ref smp-module Multicore support
///   - \ref fiber-module Utilities for managing loosely coupled chains of
///          continuations, also known as fibers
///   - \ref thread-module Support for traditional threaded execution
///   - \ref rpc Build high-level communication protocols
///   - \ref websocket (experimental) Implement a WebSocket-based server
///   - \ref fsnotifier (experimental) Implement a filesystem modification notifier.
///
/// View the [Seastar compatibility statement](./md_compatibility.html) for
/// information about library evolution.

#include <seastar/core/sstring.hh>
#include <seastar/core/future.hh>
#include <seastar/core/file-types.hh>
#include <seastar/core/posix.hh>
#include <seastar/util/bool_class.hh>
#include <seastar/util/std-compat.hh>
#include <seastar/util/modules.hh>
#include "./internal/api-level.hh"
#ifndef SEASTAR_MODULE
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>
#endif

namespace seastar {

SEASTAR_MODULE_EXPORT_BEGIN

// iostream.hh
template <class CharType> class input_stream;
template <class CharType> class output_stream;

class server_socket;
class socket;
class connected_socket;
class socket_address;
struct listen_options;
enum class transport;

// file.hh
class file;
struct file_open_options;
struct stat_data;

namespace net {

using udp_channel = class datagram_channel;

}

namespace experimental {
// process.hh
class process;
struct spawn_parameters;
}

// Networking API

/// \defgroup networking-module Networking
///
/// Seastar provides a simple networking API, backed by two
/// TCP/IP stacks: the POSIX stack, utilizing the kernel's
/// BSD socket APIs, and the native stack, implement fully
/// within seastar and able to drive network cards directly.
/// The native stack supports zero-copy on both transmit
/// and receive, and is implemented using seastar's high
/// performance, lockless sharded design.  The network stack
/// can be selected with the \c \--network-stack command-line
/// parameter.

/// \addtogroup networking-module
/// @{

/// Listen for connections on a given port
///
/// Starts listening on a given address for incoming connections.
///
/// \param sa socket address to listen on
///
/// \return \ref server_socket object ready to accept connections.
///
/// \see listen(socket_address sa, listen_options opts)
server_socket listen(socket_address sa);

/// Listen for connections on a given port
///
/// Starts listening on a given address for incoming connections.
///
/// \param sa socket address to listen on
/// \param opts options controlling the listen operation
///
/// \return \ref server_socket object ready to accept connections.
///
/// \see listen(socket_address sa)
server_socket listen(socket_address sa, listen_options opts);

/// Establishes a connection to a given address
///
/// Attempts to connect to the given address.
///
/// \param sa socket address to connect to
///
/// \return a \ref connected_socket object, or an exception
future<connected_socket> connect(socket_address sa);

/// Establishes a connection to a given address
///
/// Attempts to connect to the given address with a defined local endpoint
///
/// \param sa socket address to connect to
/// \param local socket address for local endpoint
/// \param proto transport protocol (TCP or SCTP)
///
/// \return a \ref connected_socket object, or an exception
future<connected_socket> connect(socket_address sa, socket_address local, transport proto);


/// Creates a socket object suitable for establishing stream-oriented connections
///
/// \return a \ref socket object that can be used for establishing connections
socket make_socket();

/// Creates a udp_channel object suitable for sending UDP packets
///
/// The channel is not bound to a local address, and thus can only be used
/// for sending.
///
/// \return a \ref net::udp_channel object that can be used for UDP transfers.
[[deprecated("Use `make_unbound_datagram_channel` instead")]]
net::udp_channel make_udp_channel();


/// Creates a udp_channel object suitable for sending and receiving UDP packets
///
/// \param local local address to bind to
///
/// \return a \ref net::udp_channel object that can be used for UDP transfers.
[[deprecated("Use `make_bound_datagram_channel` instead")]]
net::udp_channel make_udp_channel(const socket_address& local);

/// Creates a datagram_channel object suitable for sending datagrams to
/// destinations that belong to the provided address family.
/// Supported address families: AF_INET, AF_INET6 and AF_UNIX.
///
/// Setting family to AF_INET or AF_INET6 creates a datagram_channel that uses
/// UDP protocol. AF_UNIX creates a datagram_channel that uses UNIX domain
/// sockets.
///
/// The channel is not bound to a local address, and thus can only be used
/// for sending.
///
/// \param family address family in which the \ref datagram_channel will operate
///
/// \return a \ref net::datagram_channel object for sending datagrams in a
/// specified address family.
net::datagram_channel make_unbound_datagram_channel(sa_family_t family);

/// Creates a datagram_channel object suitable for sending and receiving
/// datagrams to/from destinations that belong to the provided address family.
/// Supported address families: AF_INET, AF_INET6 and AF_UNIX.
///
/// \param local local address to bind to
///
/// \return a \ref net::datagram_channel object for sending/receiving datagrams
/// in a specified address family.
net::datagram_channel make_bound_datagram_channel(const socket_address& local);

/// @}

/// \defgroup fileio-module File Input/Output
///
/// Seastar provides a file API to deal with persistent storage.
/// Unlike most file APIs, seastar offers unbuffered file I/O
/// (similar to, and based on, \c O_DIRECT).  Unbuffered I/O means
/// that the application is required to do its own caching, but
/// delivers better performance if this caching is done correctly.
///
/// For random I/O or sequential unbuffered I/O, the \ref file
/// class provides a set of methods for reading, writing, discarding,
/// or otherwise manipulating a file.  For buffered sequential I/O,
/// see \ref make_file_input_stream() and \ref make_file_output_stream().

/// \addtogroup fileio-module
/// @{

/// Opens or creates a file.  The "dma" in the name refers to the fact
/// that data transfers are unbuffered and uncached.
///
/// \param name  the name of the file to open or create
/// \param flags various flags controlling the open process
/// \return a \ref file object, as a future
///
/// \note
/// The file name is not guaranteed to be stable on disk, unless the
/// containing directory is sync'ed.
///
/// \relates file
future<file> open_file_dma(std::string_view name, open_flags flags) noexcept;

/// Opens or creates a file.  The "dma" in the name refers to the fact
/// that data transfers are unbuffered and uncached.
///
/// \param name  the name of the file to open or create
/// \param flags various flags controlling the open process
/// \param options options for opening the file
/// \return a \ref file object, as a future
///
/// \note
/// The file name is not guaranteed to be stable on disk, unless the
/// containing directory is sync'ed.
///
/// \relates file
future<file> open_file_dma(std::string_view name, open_flags flags, file_open_options options) noexcept;

/// Checks if a given directory supports direct io
///
/// Seastar bypasses the Operating System caches and issues direct io to the
/// underlying block devices. Projects using seastar should check if the directory
/// lies in a filesystem that support such operations. This function can be used
/// to do that.
///
/// It will return if direct io can be used, or throw an std::system_error
/// exception, with the EINVAL error code.
///
/// A std::system_error with the respective error code is also thrown if \c path is
/// not a directory.
///
/// \param path the directory we need to verify.
future<> check_direct_io_support(std::string_view path) noexcept;

/// Opens a directory.
///
/// \param name name of the directory to open
///
/// \return a \ref file object representing a directory.  The only
///    legal operations are \ref file::list_directory(),
///    \ref file::flush(), and \ref file::close().
///
/// \relates file
future<file> open_directory(std::string_view name) noexcept;

/// Creates a new directory.
///
/// \param name name of the directory to create
/// \param permissions optional file permissions of the directory to create.
///
/// \note
/// The directory is not guaranteed to be stable on disk, unless the
/// containing directory is sync'ed.
future<> make_directory(std::string_view name, file_permissions permissions = file_permissions::default_dir_permissions) noexcept;

/// Ensures a directory exists
///
/// Checks whether a directory exists, and if not, creates it.  Only
/// the last component of the directory name is created.
///
/// \param name name of the directory to potentially create
/// \param permissions optional file permissions of the directory to create.
///
/// \note
/// The directory is not guaranteed to be stable on disk, unless the
/// containing directory is sync'ed.
/// If the directory exists, the provided permissions are not applied.
future<> touch_directory(std::string_view name, file_permissions permissions = file_permissions::default_dir_permissions) noexcept;

/// Recursively ensures a directory exists
///
/// Checks whether each component of a directory exists, and if not, creates it.
///
/// \param name name of the directory to potentially create
/// \param permissions optional file permissions of the directory to create.
///
/// \note
/// This function fsyncs each component created, and is therefore guaranteed to be stable on disk.
/// The provided permissions are applied only on the last component in the path, if it needs to be created,
/// if intermediate directories do not exist, they are created with the default_dir_permissions.
/// If any directory exists, the provided permissions are not applied.
future<> recursive_touch_directory(std::string_view name, file_permissions permissions = file_permissions::default_dir_permissions) noexcept;

/// Synchronizes a directory to disk
///
/// Makes sure the modifications in a directory are synchronized in disk.
/// This is useful, for instance, after creating or removing a file inside the
/// directory.
///
/// \param name name of the directory to potentially create
future<> sync_directory(std::string_view name) noexcept;


/// Removes (unlinks) a file or an empty directory
///
/// \param name name of the file or the directory to remove
///
/// \note
/// The removal is not guaranteed to be stable on disk, unless the
/// containing directory is sync'ed.
future<> remove_file(std::string_view name) noexcept;

/// Renames (moves) a file.
///
/// \param old_name existing file name
/// \param new_name new file name
///
/// \note
/// The rename is not guaranteed to be stable on disk, unless the
/// both containing directories are sync'ed.
future<> rename_file(std::string_view old_name, std::string_view new_name) noexcept;

struct follow_symlink_tag { };
using follow_symlink = bool_class<follow_symlink_tag>;

/// Return stat information about a file.
///
/// \param name name of the file to return its stat information
/// \param fs a follow_symlink flag to follow symbolic links.
///
/// \return stat_data of the file identified by name.
/// If name identifies a symbolic link then stat_data is returned either for the target of the link,
/// with follow_symlink::yes, or for the link itself, with follow_symlink::no.
future<stat_data> file_stat(std::string_view name, follow_symlink fs = follow_symlink::yes) noexcept;

/// Wrapper around getgrnam_r.
/// If the provided group name does not exist in the group database, this call will return an empty optional.
/// If the provided group name exists in the group database, the optional returned will contain the struct group_details information.
/// When an unexpected error is thrown by the getgrnam_r libc call, this function throws std::system_error with std::error_code.
/// \param groupname name of the group
///
/// \return optional struct group_details of the group identified by name. struct group_details has details of the group from the group database.
future<std::optional<struct group_details>> getgrnam(std::string_view name);

/// Change the owner and group of file. This is a wrapper around chown syscall.
/// The function throws std::system_error, when the chown syscall fails.
/// \param filepath
/// \param owner
/// \param group
future<> chown(std::string_view filepath, uid_t owner, gid_t group);
/// Return the size of a file.
///
/// \param name name of the file to return the size
///
/// Note that file_size of a symlink is NOT the size of the symlink -
/// which is the length of the pathname it contains -
/// but rather the size of the file to which it points.
future<uint64_t> file_size(std::string_view name) noexcept;

/// Check file access.
///
/// \param name name of the file to check
/// \param flags bit pattern containing type of access to check (read/write/execute or exists).
///
/// If only access_flags::exists is queried, returns true if the file exists, or false otherwise.
/// Throws a std::filesystem::filesystem_error exception if any error other than ENOENT is encountered.
///
/// If any of the access_flags (read/write/execute) is set, returns true if the file exists and is
/// accessible with the requested flags, or false if the file exists and is not accessible
/// as queried.
/// Throws a std::filesystem::filesystem_error exception if any error other than EACCES is encountered.
/// Note that if any path component leading to the file is not searchable, the file is considered inaccessible
/// with the requested mode and false will be returned.
future<bool> file_accessible(std::string_view name, access_flags flags) noexcept;

/// check if a file exists.
///
/// \param name name of the file to check
future<bool> file_exists(std::string_view name) noexcept;

/// Determine the type of a file (regular file, directory, etc.)
///
/// \param name name of the file for which type information is requested
/// \param follow a follow_symlink flag that determines whether a trailing symbolic link should be followed or not
///
/// \return a engaged optional with the file type if lookup was successful; a disengaged optional
///      if the file (or one of its parent directories) does not exist; an exceptional future on
///      other errors.
future<std::optional<directory_entry_type>> file_type(std::string_view name, follow_symlink follow = follow_symlink::yes) noexcept;


/// Creates a hard link for a file
///
/// \param oldpath existing file name
/// \param newpath name of link
///
future<> link_file(std::string_view oldpath, std::string_view newpath) noexcept;

/// Changes the permissions mode of a file or directory
///
/// \param name name of the file ot directory to change
/// \param permissions permissions to set
///
future<> chmod(std::string_view name, file_permissions permissions) noexcept;

/// Return information about the filesystem where a file is located.
///
/// \param name name of the file to inspect
future<fs_type> file_system_at(std::string_view name) noexcept;

/// Return space available to unprivileged users in filesystem where a file is located, in bytes.
///
/// \param name name of the file to inspect
future<uint64_t> fs_avail(std::string_view name) noexcept;

/// Return free space in filesystem where a file is located, in bytes.
///
/// \param name name of the file to inspect
future<uint64_t> fs_free(std::string_view name) noexcept;
/// @}

/// Return filesystem-wide space_info where a file is located.
///
/// \param name name of the file in the filesystem to inspect
future<std::filesystem::space_info> file_system_space(std::string_view name) noexcept;

namespace experimental {
/// \defgroup interprocess-module Interprocess Communication
///
/// Seastar provides a set of APIs for interprocess communicate

/// \addtogroup interprocess-module
/// @{

/// Create a pipe using \c pipe2
///
/// \return a tuple of \c file_desc, the first one for reading from the pipe, the second
/// for writing to it.
future<std::tuple<file_desc, file_desc>> make_pipe();

/// Spawn a subprocess
///
/// \param pathname the path to the executable
/// \param params parameters for spawning the subprocess
///
/// \return a process representing the spawned subprocess
/// \note
/// the subprocess is spawned with \c posix_spawn() system call, so the
/// pathname should be relative or absolute path of the executable.
future<process> spawn_process(const std::filesystem::path& pathname,
                              spawn_parameters params);
/// Spawn a subprocess
///
/// \param pathname the path to the executable
///
/// \return a process representing the spawned subprocess
/// \note
/// the this overload does not specify a \c params parameters for spawning the
/// subprocess. Instead, it uses the pathname for the \c argv[0] in the params.
future<process> spawn_process(const std::filesystem::path& pathname);
/// @}
}

SEASTAR_MODULE_EXPORT_END

}
