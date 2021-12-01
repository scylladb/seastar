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
 * Copyright (C) 2017 ScyllaDB
 */

#pragma once

#include <seastar/core/shared_ptr.hh>


/// \file
/// \brief Include this header files when using \c lw_shared_ptr<some_incomplete_type>, at the point
/// where \c some_incomplete_type is defined.

namespace seastar {

namespace internal {

// Overload when lw_shared_ptr_deleter<T> specialized
template <typename T>
T*
lw_shared_ptr_accessors<T, void_t<decltype(lw_shared_ptr_deleter<T>{})>>::to_value(lw_shared_ptr_counter_base* counter) {
    return static_cast<T*>(counter);
}

}

}
