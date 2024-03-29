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
 * Copyright 2015 Cloudius Systems
 */

#pragma once

#ifndef SEASTAR_MODULE
#include <fcntl.h>
#include <sys/stat.h>
#include <type_traits>
#include <seastar/util/modules.hh>
#endif

namespace seastar {

SEASTAR_MODULE_EXPORT_BEGIN

/// \addtogroup fileio-module
/// @{

/// Enumeration describing how a file is to be opened.
///
/// \see file::open_file_dma()
enum class open_flags {
    rw = O_RDWR,
    ro = O_RDONLY,
    wo = O_WRONLY,
    create = O_CREAT,
    truncate = O_TRUNC,
    exclusive = O_EXCL,
    dsync = O_DSYNC,
};

inline open_flags operator|(open_flags a, open_flags b) {
    return open_flags(std::underlying_type_t<open_flags>(a) | std::underlying_type_t<open_flags>(b));
}

inline void operator|=(open_flags& a, open_flags b) {
    a = (a | b);
}

inline open_flags operator&(open_flags a, open_flags b) {
    return open_flags(std::underlying_type_t<open_flags>(a) & std::underlying_type_t<open_flags>(b));
}

inline void operator&=(open_flags& a, open_flags b) {
    a = (a & b);
}

/// Enumeration describing the type of a directory entry being listed.
///
/// \see file::list_directory()
enum class directory_entry_type {
    unknown,
    block_device,
    char_device,
    directory,
    fifo,
    link,
    regular,
    socket,
};

namespace internal::linux_abi {

// From getdents(2):
// check for 64-bit inode number
static_assert(sizeof(ino_t) == 8, "large file support not enabled");
static_assert(sizeof(off_t) == 8, "large file support not enabled");

// From getdents(2):
struct linux_dirent64 {
    ino_t          d_ino;    /* 64-bit inode number */
    off_t          d_off;    /* 64-bit offset to next structure */
    unsigned short d_reclen; /* Size of this dirent */
    unsigned char  d_type;   /* File type */
    char           d_name[]; /* Filename (null-terminated) */
};

} // internal::linux_abi namespace

/// Enumeration describing the type of a particular filesystem
enum class fs_type {
    other,
    xfs,
    ext2,
    ext3,
    ext4,
    btrfs,
    hfs,
    tmpfs,
};

// Access flags for files/directories
enum class access_flags {
    exists = F_OK,
    read = R_OK,
    write = W_OK,
    execute = X_OK,

    // alias for directory access
    lookup = execute,
};

inline access_flags operator|(access_flags a, access_flags b) {
    return access_flags(std::underlying_type_t<access_flags>(a) | std::underlying_type_t<access_flags>(b));
}

inline access_flags operator&(access_flags a, access_flags b) {
    return access_flags(std::underlying_type_t<access_flags>(a) & std::underlying_type_t<access_flags>(b));
}

// Permissions for files/directories
enum class file_permissions {
    user_read = S_IRUSR,        // Read by owner
    user_write = S_IWUSR,       // Write by owner
    user_execute = S_IXUSR,     // Execute by owner

    group_read = S_IRGRP,       // Read by group
    group_write = S_IWGRP,      // Write by group
    group_execute = S_IXGRP,    // Execute by group

    others_read = S_IROTH,      // Read by others
    others_write = S_IWOTH,     // Write by others
    others_execute = S_IXOTH,   // Execute by others

    user_permissions = user_read | user_write | user_execute,
    group_permissions = group_read | group_write | group_execute,
    others_permissions = others_read | others_write | others_execute,
    all_permissions = user_permissions | group_permissions | others_permissions,

    default_file_permissions = user_read | user_write | group_read | group_write | others_read | others_write, // 0666
    default_dir_permissions = all_permissions, // 0777
};

inline constexpr file_permissions operator|(file_permissions a, file_permissions b) {
    return file_permissions(std::underlying_type_t<file_permissions>(a) | std::underlying_type_t<file_permissions>(b));
}

inline constexpr file_permissions operator&(file_permissions a, file_permissions b) {
    return file_permissions(std::underlying_type_t<file_permissions>(a) & std::underlying_type_t<file_permissions>(b));
}

/// @}

SEASTAR_MODULE_EXPORT_END

} // namespace seastar
