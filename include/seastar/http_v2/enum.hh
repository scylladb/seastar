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

#pragma once

#include <seastar/core/sstring.hh>
#include <seastar/util/trie.hh>
#include <utility>

namespace seastar::http_v2 {

/**
 * enum base class
 */
class enum_object {
    sstring name;
public:
    explicit enum_object(sstring name);
    inline sstring& get_name();
};

/**
 * http method enums
 */
class method : public enum_object {
private:
    explicit method(const sstring& name);
public:
    const static method GET, HEAD, POST, PUT, DELETE, CONNECT, OPTIONS, TRACE;
    static method* find_by_name(sstring& name);
};

/**
 * http protocol version enums
 */
class version : public enum_object {
private:
    explicit version(const sstring& name);
public:
    const static version HTTP1_0, HTTP1_1;
    static version* find_by_name(sstring& name);
};

}