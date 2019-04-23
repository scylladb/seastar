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

#include <fcntl.h>
#include <sys/stat.h>

namespace seastar {

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

/// \addtogroup fileio-module
/// @{

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

/// @}

} // namespace seastar
