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
 * Copyright (C) 2016 ScyllaDB.
 */

#pragma once
#include <setjmp.h>

namespace seastar {
/// \cond internal
class thread_context;

struct jmp_buf_link {
#ifdef ASAN_ENABLED
    ucontext_t context;
#else
    jmp_buf jmpbuf;
#endif
    jmp_buf_link* link;
    thread_context* thread;
};
extern thread_local jmp_buf_link* g_current_context;

namespace thread_impl {

inline thread_context* get() {
    return g_current_context->thread;
}

void switch_in(thread_context* to);
void switch_out(thread_context* from);
void init();

}
}
/// \endcond


