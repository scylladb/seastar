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
 * Copyright (C) 2015 Scylladb, Ltd.
 */

#pragma once

#include <endian.h>
#include "unaligned.hh"

namespace seastar {

inline uint8_t cpu_to_le(uint8_t x) { return x; }
inline uint8_t le_to_cpu(uint8_t x) { return x; }
inline uint16_t cpu_to_le(uint16_t x) { return htole16(x); }
inline uint16_t le_to_cpu(uint16_t x) { return le16toh(x); }
inline uint32_t cpu_to_le(uint32_t x) { return htole32(x); }
inline uint32_t le_to_cpu(uint32_t x) { return le32toh(x); }
inline uint64_t cpu_to_le(uint64_t x) { return htole64(x); }
inline uint64_t le_to_cpu(uint64_t x) { return le64toh(x); }

inline int8_t cpu_to_le(int8_t x) { return x; }
inline int8_t le_to_cpu(int8_t x) { return x; }
inline int16_t cpu_to_le(int16_t x) { return htole16(x); }
inline int16_t le_to_cpu(int16_t x) { return le16toh(x); }
inline int32_t cpu_to_le(int32_t x) { return htole32(x); }
inline int32_t le_to_cpu(int32_t x) { return le32toh(x); }
inline int64_t cpu_to_le(int64_t x) { return htole64(x); }
inline int64_t le_to_cpu(int64_t x) { return le64toh(x); }

template <typename T>
inline T cpu_to_le(const unaligned<T>& v) {
    return cpu_to_le(T(v));
}

template <typename T>
inline T le_to_cpu(const unaligned<T>& v) {
    return le_to_cpu(T(v));
}

} // namespace seastar
