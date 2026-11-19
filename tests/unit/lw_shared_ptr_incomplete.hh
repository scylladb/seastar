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
 * Copyright (C) 2026 ScyllaDB
 */

// Declarations of the types that lw_shared_ptr_incomplete_test.cc uses
// without seeing their definitions, which are in
// lw_shared_ptr_incomplete_types.cc.

#pragma once

#include <seastar/core/shared_ptr.hh>

namespace incomplete {

// Counts the destructions of the types below.
extern int destroyed;

// The count is next to the object, in a wrapper.
struct plain;
seastar::lw_shared_ptr<plain> make_plain(int x);
int value(const seastar::lw_shared_ptr<const plain>& p);

// The count is in the object, in its enable_lw_shared_from_this base.
struct with_esft;
seastar::lw_shared_ptr<with_esft> make_with_esft(int x);
int value(const seastar::lw_shared_ptr<with_esft>& p);
seastar::lw_shared_ptr<with_esft> shared_from_this(const seastar::lw_shared_ptr<with_esft>& p);

// When its last pointer goes away, it's counted, and destroyed at once.
struct hooked;
void lw_shared_ptr_dispose(hooked* p) noexcept;
seastar::lw_shared_ptr<hooked> make_hooked();
extern int hooked_disposals;

// When its last pointer goes away, it waits for dispose_deferred().
struct deferred;
void lw_shared_ptr_dispose(deferred* p) noexcept;
seastar::lw_shared_ptr<deferred> make_deferred();
void dispose_deferred();

}

extern template class seastar::lw_shared_ptr<incomplete::plain>;
extern template class seastar::lw_shared_ptr<incomplete::with_esft>;
extern template class seastar::lw_shared_ptr<incomplete::hooked>;
extern template class seastar::lw_shared_ptr<incomplete::deferred>;

// The old way, which predates the explicit instantiation above, and still
// works without Seastar's C++ module: a specialization of
// lw_shared_ptr_deleter, a type that derives from enable_lw_shared_from_this,
// and shared_ptr_incomplete.hh where the type is defined. If this way is ever
// deprecated and removed, remove its test too.
namespace incomplete {
struct old_way;
seastar::lw_shared_ptr<old_way> make_old_way();
extern int old_way_disposals;
}

template <>
struct seastar::lw_shared_ptr_deleter<incomplete::old_way> {
    static void dispose(incomplete::old_way* p);
};
