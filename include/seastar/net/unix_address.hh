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
 * Copyright (C) 2019 Red Hat, Inc.
 */
#pragma once

#ifndef SEASTAR_MODULE
#include <iosfwd>
#include <sys/types.h>
#include <sys/un.h>
#include <string>
#endif
#include <seastar/util/modules.hh>

namespace seastar {
SEASTAR_MODULE_EXPORT_BEGIN
/*!
    A helper struct for creating/manipulating UNIX-domain sockets.

    A UNIX-domain socket is either named or unnamed. If named, the name is either
    a path in the filesystem namespace, or an abstract-domain identifier. Abstract-domain
    names start with a null byte, and may contain non-printable characters.

    std::string() can hold a sequence of arbitrary bytes, and has a length() attribute
    that does not rely on using strlen(). Thus it is used here to hold the address.
 */
struct unix_domain_addr {
    const std::string name;
    const int path_count;  //  either name.length() or name.length()+1. See path_length_aux() below.

    explicit unix_domain_addr(const std::string& fn) : name{fn}, path_count{path_length_aux()} {}

    explicit unix_domain_addr(const char* fn) : name{fn}, path_count{path_length_aux()} {}

    int path_length() const { return path_count; }

    //  the following holds:
    //  for abstract name: name.length() == number of meaningful bytes, including the null in name[0].
    //  for filesystem path: name.length() does not count the implicit terminating null.
    //  Here we tweak the outside-visible length of the address.
    int path_length_aux() const {
        auto pl = (int)name.length();
        if (!pl || (name[0] == '\0')) {
            // unnamed, or abstract-namespace
            return pl;
        }
        return 1 + pl;
    }

    const char* path_bytes() const { return name.c_str(); }

    bool operator==(const unix_domain_addr& a) const {
        return name == a.name;
    }
    bool operator!=(const unix_domain_addr& a) const {
        return !(*this == a);
    }
};

std::ostream& operator<<(std::ostream&, const unix_domain_addr&);
SEASTAR_MODULE_EXPORT_END
} // namespace seastar
