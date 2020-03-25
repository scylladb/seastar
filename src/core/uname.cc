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

#include "uname.hh"
#include <regex>
#include <boost/algorithm/cxx11/any_of.hpp>
#include <sys/utsname.h>
#include <iostream>

namespace seastar {

namespace internal {

int uname_t::component_count() const {
    if (distro_patch) {
        return 5;
    }
    if (subsublevel) {
        return 4;
    }
    if (sublevel) {
        return 3;
    }
    return 2;
}

bool uname_t::has_distro_extra(std::string extra) const {
    return distro_extra.find(extra) != std::string::npos;
}

// Can't use optional compares, C++17 only
static int cmp(const compat::optional<int>& u1, const compat::optional<int>& u2) {
    return int(u1.value_or(0) - u2.value_or(0));
}

bool uname_t::same_as_or_descendant_of(const uname_t& x) const {
    if (version < x.version) {
        return false; // 4.2 vs. 5.1
    }
    if (version == x.version && patchlevel < x.patchlevel) {
        return false; // 4.0 vs 4.1
    }
    if (!has_distro_extra(x.distro_extra)) {
        return false;
    }
    switch (x.component_count()) {
    case 5:
        return version == x.version
                && patchlevel == x.patchlevel
                && cmp(sublevel, x.sublevel) == 0
                && cmp(subsublevel, x.subsublevel) == 0
                && cmp(distro_patch, x.distro_patch) >= 0;
    case 4:
        return version == x.version
                && patchlevel == x.patchlevel
                && cmp(sublevel, x.sublevel) == 0
                && cmp(subsublevel, x.subsublevel) >= 0;
    case 3:
        return version == x.version
                && patchlevel == x.patchlevel
                && cmp(sublevel, x.sublevel) >= 0;
    case 2:
        return true;
    default:
        return false;
    }
}

uname_t parse_uname(const char* u) {
    static std::regex re(R"XX((\d+)\.(\d+)(?:\.(\d+)(?:\.(\d+))?)?(?:-(\d*)(.+))?)XX");
    std::cmatch m;
    if (std::regex_match(u, m, re)) {
        auto num = [] (std::csub_match sm) -> compat::optional<int> {
            if (sm.length() > 0) {
                return std::atoi(sm.str().c_str());
            } else {
                return compat::nullopt;
            }
        };
        return uname_t{*num(m[1]), *num(m[2]), num(m[3]), num(m[4]), num(m[5]), m[6].str()};
    } else {
        return uname_t{0, 0, compat::nullopt, compat::nullopt, compat::nullopt, ""};
    }
}


bool uname_t::whitelisted(std::initializer_list<const char*> wl) const {
    return boost::algorithm::any_of(wl, [this] (const char* v) {
        return same_as_or_descendant_of(parse_uname(v));
    });
}

std::ostream& operator<<(std::ostream& os, const uname_t& u) {
    os << u.version << "." << u.patchlevel;
    if (u.sublevel) {
        os << "." << *u.sublevel;
    }
    if (u.subsublevel) {
        os << "." << *u.subsublevel;
    }
    if (u.distro_patch || !u.distro_extra.empty()) {
        os << "-";
    }
    if (u.distro_patch) {
        os << *u.distro_patch;
    }
    os << u.distro_extra;
    return os;
}


uname_t kernel_uname() {
    struct ::utsname buf;
    ::uname(&buf);
    return parse_uname(buf.release);
}

}
}
