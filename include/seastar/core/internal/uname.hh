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
 * Copyright (C) 2019 ScyllaDB
 */

#pragma once

#include <string>
#include <initializer_list>
#include <seastar/util/std-compat.hh>
#include <iosfwd>

namespace seastar {

namespace internal {

// Representation of a Linux kernel version number
struct uname_t {
    int version;   // 4 in "4.5"
    int patchlevel;   // 5 in "4.5"
    std::optional<int> sublevel;   // 1 in "4.5.1"
    std::optional<int> subsublevel;  // 33 in "2.6.44.33"
    std::optional<int> distro_patch; // 957 in "3.10.0-957.5.1.el7.x86_64"
    std::string distro_extra; // .5.1.el7.x86_64

    bool same_as_or_descendant_of(const uname_t& x) const;
    bool same_as_or_descendant_of(const char* x) const;
    bool whitelisted(std::initializer_list<const char*>) const;

    // 3 for "4.5.0", 5 for "5.1.3-33.3.el7"; "el7" doesn't count as a component
    int component_count() const;

    // The "el7" that wasn't counted in components()
    bool has_distro_extra(std::string extra) const;
    friend std::ostream& operator<<(std::ostream& os, const uname_t& u);
};

uname_t kernel_uname();

uname_t parse_uname(const char* u);

}
}
