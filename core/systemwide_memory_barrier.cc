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
 * Copyright 2015 Scylla DB
 */

#include "systemwide_memory_barrier.hh"
#include <sys/mman.h>
#include <unistd.h>
#include <cassert>
#ifdef HAVE_SYS_MEMBARRIER
#include <linux/membarrier.h>
#include <linux/unistd.h>
#include <sys/syscall.h>
#endif

// cause all threads to invoke a full memory barrier (mprotect version)
void
systemwide_memory_barrier_mprotect() {
    static thread_local char* mem = [] {
       void* mem = mmap(nullptr, getpagesize(),
               PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS,
               -1, 0) ;
       assert(mem != MAP_FAILED);
       return reinterpret_cast<char*>(mem);
    }();
    int r1 = mprotect(mem, getpagesize(), PROT_READ | PROT_WRITE);
    assert(r1 == 0);
    // Force page into memory to avoid next mprotect() attempting to be clever
    *mem = 3;
    // Force page into memory
    // lower permissions to force kernel to send IPI to all threads, with
    // a side effect of executing a memory barrier on those threads
    // FIXME: does this work on ARM?
    int r2 = mprotect(mem, getpagesize(), PROT_READ);
    assert(r2 == 0);
}

#ifdef HAVE_SYS_MEMBARRIER

// cause all threads to invoke a full memory barrier (sys_membarrier version)
void
systemwide_memory_barrier_syscall() {
    int r = syscall(__NR_membarrier, MEMBARRIER_CMD_SHARED, 0);
    assert(r == 0);
}

#endif

using systemwide_memory_barrier_fn_type = void (*)();

static
systemwide_memory_barrier_fn_type
select_systemwide_memory_barrier_implementation() {
#ifdef HAVE_SYS_MEMBARRIER
    int r1 = syscall(__NR_membarrier, MEMBARRIER_CMD_QUERY, 0);
    if (r1 >= 0 && (r1 & MEMBARRIER_CMD_SHARED)) {
        return systemwide_memory_barrier_syscall;
    }
#endif
    return systemwide_memory_barrier_mprotect;
}

static systemwide_memory_barrier_fn_type systemwide_memory_barrier_fn
    = select_systemwide_memory_barrier_implementation();

// cause all threads to invoke a full memory barrier
void
systemwide_memory_barrier() {
    (*systemwide_memory_barrier_fn)();
}
