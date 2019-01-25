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

#include <algorithm>
#include <endian.h>
#include <seastar/core/unaligned.hh>

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

inline uint8_t cpu_to_be(uint8_t x) { return x; }
inline uint8_t be_to_cpu(uint8_t x) { return x; }
inline uint16_t cpu_to_be(uint16_t x) { return htobe16(x); }
inline uint16_t be_to_cpu(uint16_t x) { return be16toh(x); }
inline uint32_t cpu_to_be(uint32_t x) { return htobe32(x); }
inline uint32_t be_to_cpu(uint32_t x) { return be32toh(x); }
inline uint64_t cpu_to_be(uint64_t x) { return htobe64(x); }
inline uint64_t be_to_cpu(uint64_t x) { return be64toh(x); }

inline int8_t cpu_to_be(int8_t x) { return x; }
inline int8_t be_to_cpu(int8_t x) { return x; }
inline int16_t cpu_to_be(int16_t x) { return htobe16(x); }
inline int16_t be_to_cpu(int16_t x) { return be16toh(x); }
inline int32_t cpu_to_be(int32_t x) { return htobe32(x); }
inline int32_t be_to_cpu(int32_t x) { return be32toh(x); }
inline int64_t cpu_to_be(int64_t x) { return htobe64(x); }
inline int64_t be_to_cpu(int64_t x) { return be64toh(x); }

template <typename T>
inline T cpu_to_le(const unaligned<T>& v) {
    return cpu_to_le(T(v));
}

template <typename T>
inline T le_to_cpu(const unaligned<T>& v) {
    return le_to_cpu(T(v));
}

template <typename T>
inline
T
read_le(const char* p) {
    T datum;
    std::copy_n(p, sizeof(T), reinterpret_cast<char*>(&datum));
    return le_to_cpu(datum);
}

template <typename T>
inline
void
write_le(char* p, T datum) {
    datum = cpu_to_le(datum);
    std::copy_n(reinterpret_cast<const char*>(&datum), sizeof(T), p);
}

template <typename T>
inline
T
read_be(const char* p) {
    T datum;
    std::copy_n(p, sizeof(T), reinterpret_cast<char*>(&datum));
    return be_to_cpu(datum);
}

template <typename T>
inline
void
write_be(char* p, T datum) {
    datum = cpu_to_be(datum);
    std::copy_n(reinterpret_cast<const char*>(&datum), sizeof(T), p);
}

template <typename T>
inline
T
consume_be(const char*& p) {
    auto ret = read_be<T>(p);
    p += sizeof(T);
    return ret;
}

template <typename T>
inline
void
produce_be(char*& p, T datum) {
    write_be<T>(p, datum);
    p += sizeof(T);
}

}
