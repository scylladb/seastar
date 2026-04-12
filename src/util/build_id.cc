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
 * Copyright (C) 2019-present ScyllaDB
 */

#include <cstring>
#include <ostream>

#include <seastar/util/internal/build_id.hh>

#if __has_include(<elf.h>)
#include <elf.h>
#include <link.h>
#define HAS_ELF

#include <stddef.h>
#include <fmt/format.h>

#include <seastar/core/align.hh>
#endif

namespace seastar::internal {

#ifdef HAS_ELF

constexpr size_t max_size = 32; // // SHA-256 (32 bytes) is the largest hash algorithm supported by ld --build-id.

static const Elf64_Nhdr* get_nt_build_id(dl_phdr_info* info) {
    auto base = info->dlpi_addr;
    const auto* h = info->dlpi_phdr;
    auto num_headers = info->dlpi_phnum;
    for (int i = 0; i != num_headers; ++i, ++h) {
        if (h->p_type != PT_NOTE) {
            continue;
        }

        auto* p = reinterpret_cast<const char*>(base + h->p_vaddr);
        auto* e = p + h->p_memsz;
        while (p + sizeof(Elf64_Nhdr) <= e) {
            const auto* n = reinterpret_cast<const Elf64_Nhdr*>(p);
            if (n->n_type == NT_GNU_BUILD_ID) {
                return n;
            }

            p += sizeof(Elf64_Nhdr);

            p += n->n_namesz;
            p = align_up(p, 4);

            p += n->n_descsz;
            p = align_up(p, 4);
        }
    }

    return nullptr;
}

static int callback(dl_phdr_info* info, size_t size, void* data) {
    // size is the size of the dl_phdr_info struct passed by dl_iterate_phdr.
    if (size < offsetof(dl_phdr_info, dlpi_phnum) + sizeof(info->dlpi_phnum)) {
        fmt::print(stderr, "warning: build_id: dl_iterate_phdr callback size {} is too small\n", size);
        return -1;
    }

    char* buf = static_cast<char*>(data);

    auto* n = get_nt_build_id(info);
    if (!n) {
        fmt::print(stderr, "warning: build_id: no NT_GNU_BUILD_ID note\n");
        return -1;
    }

    if (n->n_descsz > max_size) {
        fmt::print(stderr, "warning: build_id: n_descsz {} exceeds max {}\n",
                   n->n_descsz, max_size);
        return -1;
    }

    auto* p = reinterpret_cast<const unsigned char*>(n);

    p += sizeof(Elf64_Nhdr);

    p += n->n_namesz;
    p = align_up(p, 4);

    for (size_t i = 0; i < n->n_descsz; ++i) {
        fmt::format_to(buf + 2 * i, "{:02x}", p[i]);
    }
    buf[2 * n->n_descsz] = '\0';
    return n->n_descsz;
}

struct build_id {
    char buf[2 * max_size + 1];

    build_id() {
        if (dl_iterate_phdr(callback, &buf) < 0) {
            std::strcpy(buf, "unknown");
        }
    }

    const char* get() const {
        return buf;
    }
};

#else // HAS_ELF

struct build_id {
    const char* get() const {
        return "unknown";
    }
};

#endif // HAS_ELF

const char* get_build_id() {
    static internal::build_id cache;
    return cache.get();
}

} // namespace seastar::internal
