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

#include <seastar/http_v2/enum.hh>

namespace seastar::http_v2 {

/*
 * enum_object
 */
enum_object::enum_object(seastar::sstring name)
    : name(std::move(name)) {}

sstring &enum_object::get_name() {
    return name;
}

/*
 * http method
 */
static trie<method*> method_records;

method::method(const sstring& name)
    : enum_object(name)
{
    method_records.put(name, this);
}

method* method::find_by_name(sstring &name) {
    auto* node = method_records.get(name);
    if (node != nullptr) {
        return node->getValue();
    } else {
        return nullptr;
    }
}

const method method::GET = method("GET");
const method method::HEAD = method("HEAD");
const method method::POST = method("POST");
const method method::PUT = method("PUT");
const method method::DELETE = method("DELETE");
const method method::CONNECT = method("CONNECT");
const method method::OPTIONS = method("OPTIONS");
const method method::TRACE = method("TRACE");

/*
 * http protocol version
 */
static trie<version*> version_records;

version::version(const seastar::sstring& name)
    : enum_object(name)
{
    version_records.put(name, this);
}

version* version::find_by_name(seastar::sstring &name) {
    auto* node = version_records.get(name);
    if (node != nullptr) {
        return node->getValue();
    } else {
        return nullptr;
    }
}

const version version::HTTP1_0 = version("HTTP/1.0");
const version version::HTTP1_1 = version("HTTP/1.1");

}
