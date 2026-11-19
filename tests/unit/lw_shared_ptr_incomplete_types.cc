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

// The definitions of the types that lw_shared_ptr_incomplete_test.cc uses
// without seeing them, and the code of their lw_shared_ptr.

#include "lw_shared_ptr_incomplete.hh"

#include <vector>

namespace incomplete {

int destroyed = 0;
int hooked_disposals = 0;

struct plain {
    int x;
    ~plain() { destroyed++; }
};

struct with_esft : seastar::enable_lw_shared_from_this<with_esft> {
    int x;
    explicit with_esft(int x) : x(x) {}
    ~with_esft() { destroyed++; }
};

struct hooked {
    ~hooked() { destroyed++; }
};

struct deferred : seastar::enable_lw_shared_from_this<deferred> {
    ~deferred() { destroyed++; }
};

seastar::lw_shared_ptr<plain> make_plain(int x) {
    return seastar::make_lw_shared<plain>(x);
}

int value(const seastar::lw_shared_ptr<const plain>& p) {
    return p->x;
}

seastar::lw_shared_ptr<with_esft> make_with_esft(int x) {
    return seastar::make_lw_shared<with_esft>(x);
}

int value(const seastar::lw_shared_ptr<with_esft>& p) {
    return p->x;
}

seastar::lw_shared_ptr<with_esft> shared_from_this(const seastar::lw_shared_ptr<with_esft>& p) {
    return p->shared_from_this();
}

void lw_shared_ptr_dispose(hooked* p) noexcept {
    hooked_disposals++;
    seastar::lw_shared_ptr<hooked>::dispose(p);
}

seastar::lw_shared_ptr<hooked> make_hooked() {
    return seastar::make_lw_shared<hooked>();
}

static std::vector<deferred*> waiting;

void lw_shared_ptr_dispose(deferred* p) noexcept {
    waiting.push_back(p);
}

void dispose_deferred() {
    for (auto* p : waiting) {
        seastar::lw_shared_ptr<deferred>::dispose(p);
    }
    waiting.clear();
}

seastar::lw_shared_ptr<deferred> make_deferred() {
    return seastar::make_lw_shared<deferred>();
}

}

template class seastar::lw_shared_ptr<incomplete::plain>;
template class seastar::lw_shared_ptr<incomplete::with_esft>;
template class seastar::lw_shared_ptr<incomplete::hooked>;
template class seastar::lw_shared_ptr<incomplete::deferred>;

// The old way (see lw_shared_ptr_incomplete.hh).
#include <seastar/core/shared_ptr_incomplete.hh>

namespace incomplete {

int old_way_disposals = 0;

struct old_way : seastar::enable_lw_shared_from_this<old_way> {
    ~old_way() { destroyed++; }
};

seastar::lw_shared_ptr<old_way> make_old_way() {
    return seastar::make_lw_shared<old_way>();
}

}

void seastar::lw_shared_ptr_deleter<incomplete::old_way>::dispose(incomplete::old_way* p) {
    incomplete::old_way_disposals++;
    delete p;
}

template incomplete::old_way*
seastar::internal::lw_shared_ptr_accessors<incomplete::old_way, void>::to_value(seastar::lw_shared_ptr_counter_base*);
