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

// The purpose of the hacks here is to workaround C++ exception scalability problem
// with gcc and glibc. For the best result gcc-7 is required.
//
// To summarize all the locks we have now and their purpose:
// 1. There is a lock in _Unwind_Find_FDE (libgcc) that protects
//    list of "Frame description entries" registered with __register_frame*
//    functions. The catch is that dynamically linked binary do not do that,
//    so all that it protects is checking that a certain list is empty.
//    This lock no longer relevant in gcc-7 since there is a patch there
//    that checks that the list is empty outside of the lock and it will be
//    always true for us.
// 2. The lock in dl_iterate_phdr (glibc) that protects loaded object
//    list against runtime object loading/unloading.
//
// To get rid of the first lock using gcc-7 is required.
//
// To get rid of the second one we can use the fact that we do not
// load/unload objects dynamically (at least for now). To do that we
// can mirror all elf header information in seastar and provide our
// own dl_iterate_phdr symbol which uses this mirror without locking.
//
// Unfortunately there is another gotcha in this approach: dl_iterate_phdr
// supplied by glibc never calls more then one callback simultaneously as an
// unintended consequences of the lock there, but unfortunately libgcc relies
// on that to maintain small cache of translations. The access to the cache is
// not protected by any lock since up until now only one callback could have
// run at a time. But luckily libgcc cannot use the cache if older version
// of dl_phdr_info is provided to the callback because the older version
// did not have an indication that loaded object list may have changed,
// so libgcc does not know when cache should be invalidated and disables it
// entirely. By calling the callback with old version of dl_phdr_info from
// our dl_iterate_phdr we can effectively make libgcc callback thread safe.

#ifdef SEASTAR_MODULE
module;
#endif

#include <link.h>
#include <dlfcn.h>
#include <vector>
#include <cstddef>

#ifdef SEASTAR_MODULE
module seastar;
#else
#include <seastar/core/exception_hacks.hh>
#include <seastar/core/reactor.hh>
#include <seastar/util/backtrace.hh>
#endif
#include <seastar/util/assert.hh>

namespace seastar {
using dl_iterate_fn = int (*) (int (*callback) (struct dl_phdr_info *info, size_t size, void *data), void *data);

[[gnu::no_sanitize_address]]
static dl_iterate_fn dl_iterate_phdr_org() {
    static dl_iterate_fn org = [] {
        auto org = (dl_iterate_fn)dlsym (RTLD_NEXT, "dl_iterate_phdr");
        SEASTAR_ASSERT(org);
        return org;
    }();
    return org;
}

// phdrs_cache has to remain valid until very late in the process
// life, and that time is not a mirror image of when it is first used.
// Given that, we avoid a static constructor/destructor pair and just
// never destroy it.
static std::vector<dl_phdr_info> *phdrs_cache = nullptr;

void init_phdr_cache() {
    // this process started reactor before, no need to fill the cache
    if (phdrs_cache) {
        return;
    }
    // Fill out elf header cache for access without locking.
    // This assumes no dynamic object loading/unloading after this point
    phdrs_cache = new std::vector<dl_phdr_info>();
    dl_iterate_phdr_org()([] (struct dl_phdr_info *info, size_t size, void *data) {
        phdrs_cache->push_back(*info);
        return 0;
    }, nullptr);
}

void internal::increase_thrown_exceptions_counter() noexcept {
    seastar::engine()._cxx_exceptions++;
}

#ifndef NO_EXCEPTION_INTERCEPT
seastar::logger exception_logger("exception");

void log_exception_trace() noexcept {
    static thread_local bool nested = false;
    if (!nested && exception_logger.is_enabled(log_level::trace)) {
        nested = true;
        exception_logger.trace("Throw exception at:\n{}", current_backtrace());
        nested = false;
    }
}
#else
void log_exception_trace() noexcept {}
#endif

}

extern "C"
[[gnu::visibility("default")]]
[[gnu::used]]
[[gnu::no_sanitize_address]]
int dl_iterate_phdr(int (*callback) (struct dl_phdr_info *info, size_t size, void *data), void *data) {
    if (!seastar::phdrs_cache || !seastar::local_engine) {
        // Cache is not yet populated, pass through to original function
        return seastar::dl_iterate_phdr_org()(callback, data);
    }
    int r = 0;
    for (auto h : *seastar::phdrs_cache) {
        // Pass dl_phdr_info size that does not include dlpi_adds and dlpi_subs.
        // This forces libgcc to disable caching which is not thread safe and
        // requires dl_iterate_phdr to serialize calls to callback. Since we do
        // not serialize here we have to disable caching.
        r = callback(&h, offsetof(dl_phdr_info, dlpi_adds), data);
        if (r) {
            break;
        }
    }
    return r;
}

#ifndef NO_EXCEPTION_INTERCEPT
extern "C"
[[gnu::visibility("default")]]
[[gnu::used]]
int _Unwind_RaiseException(struct ::_Unwind_Exception *h) {
    using throw_fn =  int (*)(void *);
    static throw_fn org = nullptr;

    if (!org) {
        org = (throw_fn)dlsym (RTLD_NEXT, "_Unwind_RaiseException");
    }
    if (seastar::local_engine) {
        seastar::internal::increase_thrown_exceptions_counter();
        seastar::log_exception_trace();
    }
    return org(h);
}
#endif
