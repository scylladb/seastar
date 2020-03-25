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
 * Copyright (C) 2020 ScyllaDB Ltd.
 */

#pragma once

#include <stddef.h>
#include <type_traits>

namespace seastar {
namespace internal {
// Empty types have a size of 1, but that byte is not actually
// used. This helper is used to avoid accessing that byte.
template<typename T>
struct used_size {
    static constexpr size_t value = std::is_empty<T>::value ? 0 : sizeof(T);
};
}
}
