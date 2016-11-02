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
 * Copyright 2016 ScyllaDB
 */

#pragma once

#define UNW_LOCAL_ONLY
#include <libunwind.h>

// Invokes func for each frame passing return address as argument.
template<typename Func>
void backtrace(Func&& func) noexcept(noexcept(func(0))) {
    unw_context_t context;
    if (unw_getcontext(&context) < 0) {
        return;
    }

    unw_cursor_t cursor;
    if (unw_init_local(&cursor, &context) < 0) {
        return;
    }

    while (unw_step(&cursor) > 0) {
        unw_word_t ip;
        unw_get_reg(&cursor, UNW_REG_IP, &ip);
        func(ip);
    }
}
