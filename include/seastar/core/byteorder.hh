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
#include <boost/endian/conversion.hpp>
#include <seastar/core/unaligned.hh>

namespace seastar {

template <typename T>
inline T cpu_to_le(T x) noexcept {
  return boost::endian::native_to_little(x);
}
template <typename T>
inline T le_to_cpu(T x) noexcept {
  return boost::endian::little_to_native(x);
}

template <typename T>
inline T cpu_to_be(T x) noexcept {
  return boost::endian::native_to_big(x);
}
template <typename T>
inline T be_to_cpu(T x) noexcept {
  return boost::endian::big_to_native(x);
}

template <typename T>
inline T cpu_to_le(const unaligned<T>& v) noexcept {
    return cpu_to_le(T(v));
}

template <typename T>
inline T le_to_cpu(const unaligned<T>& v) noexcept {
    return le_to_cpu(T(v));
}

template <typename T>
inline
T
read_le(const char* p) noexcept {
    T datum;
    std::copy_n(p, sizeof(T), reinterpret_cast<char*>(&datum));
    return le_to_cpu(datum);
}

template <typename T>
inline
void
write_le(char* p, T datum) noexcept {
    datum = cpu_to_le(datum);
    std::copy_n(reinterpret_cast<const char*>(&datum), sizeof(T), p);
}

template <typename T>
inline
T
read_be(const char* p) noexcept {
    T datum;
    std::copy_n(p, sizeof(T), reinterpret_cast<char*>(&datum));
    return be_to_cpu(datum);
}

template <typename T>
inline
void
write_be(char* p, T datum) noexcept {
    datum = cpu_to_be(datum);
    std::copy_n(reinterpret_cast<const char*>(&datum), sizeof(T), p);
}

template <typename T>
inline
T
consume_be(const char*& p) noexcept {
    auto ret = read_be<T>(p);
    p += sizeof(T);
    return ret;
}

template <typename T>
inline
void
produce_be(char*& p, T datum) noexcept {
    write_be<T>(p, datum);
    p += sizeof(T);
}

}
