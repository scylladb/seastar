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
 * Copyright (C) 2022 ScyllaDB.
 */

#pragma once

#include <linux/magic.h>

namespace seastar {

#ifndef XFS_SUPER_MAGIC
#define XFS_SUPER_MAGIC 0x58465342
#endif

#ifndef EXT2_SUPER_MAGIC
#define EXT2_SUPER_MAGIC 0xEF53
#endif

#ifndef EXT3_SUPER_MAGIC
#define EXT3_SUPER_MAGIC 0xEF53
#endif

#ifndef EXT4_SUPER_MAGIC
#define EXT4_SUPER_MAGIC 0xEF53
#endif

#ifndef NFS_SUPER_MAGIC
#define NFS_SUPER_MAGIC 0x6969
#endif

#ifndef TMPFS_MAGIC
#define TMPFS_MAGIC 0x01021994
#endif

#ifndef FUSE_SUPER_MAGIC
#define FUSE_SUPER_MAGIC 0x65735546
#endif

#ifndef BTRFS_SUPER_MAGIC
#define BTRFS_SUPER_MAGIC 0x9123683E
#endif

#ifndef HFS_SUPER_MAGIC
#define HFS_SUPER_MAGIC 0x4244
#endif

namespace internal {

class fs_magic {
public:
    static constexpr unsigned long xfs = XFS_SUPER_MAGIC;
    static constexpr unsigned long ext2 = EXT2_SUPER_MAGIC;
    static constexpr unsigned long ext3 = EXT3_SUPER_MAGIC;
    static constexpr unsigned long ext4 = EXT4_SUPER_MAGIC;
    static constexpr unsigned long nfs = NFS_SUPER_MAGIC;
    static constexpr unsigned long tmpfs = TMPFS_MAGIC;
    static constexpr unsigned long fuse = FUSE_SUPER_MAGIC;
    static constexpr unsigned long btrfs = BTRFS_SUPER_MAGIC;
    static constexpr unsigned long hfs = HFS_SUPER_MAGIC;
};

} // internal namespace

} // seastar namespace
