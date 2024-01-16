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

#include <seastar/core/iostream.hh>
#include <seastar/core/sstring.hh>
#include <seastar/http_v2/common.hh>

namespace seastar::http_v2 {

/**
 * A client sends an HTTP request to a server in the form of a request
 * message, beginning with a request-line that includes a method, URI,
 * and protocol version, followed by header fields containing request
 * modifiers, client information, and representation metadata, an empty
 * line to indicate the end of the header section, and finally a message
 * body containing the payload body.
 */
class request {
    method* _method;
    sstring _uri;
    version* _version;
    std::unordered_map<sstring, sstring> headers;
private:
    explicit request()
        : _method(nullptr)
        , _version(nullptr) {}

    explicit request(method& _method, sstring& _uri, version& _version)
        : _method(&_method)
        , _uri(_uri)
        , _version(&_version) {}
public:
    method& get_method() {
        return *_method;
    }

    sstring& get_uri() {
        return _uri;
    }

    version& get_version() {
        return *_version;
    }
};

}
