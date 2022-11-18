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
 * Copyright (C) 2022 Scylladb, Ltd.
 */

#include <seastar/http/request.hh>
#include <seastar/http/url.hh>

namespace seastar {
namespace http {

void request::add_param(const std::string_view& param) {
    size_t split = param.find('=');

    if (split >= param.length() - 1) {
        sstring key;
        if (http::internal::url_decode(param.substr(0,split) , key)) {
            query_parameters[key] = "";
        }
    } else {
        sstring key;
        sstring value;
        if (http::internal::url_decode(param.substr(0,split), key)
                && http::internal::url_decode(param.substr(split + 1), value)) {
            query_parameters[key] = value;
        }
    }

}

sstring request::parse_query_param() {
    size_t pos = _url.find('?');
    if (pos == sstring::npos) {
        return _url;
    }
    size_t curr = pos + 1;
    size_t end_param;
    std::string_view url = _url;
    while ((end_param = _url.find('&', curr)) != sstring::npos) {
        add_param(url.substr(curr, end_param - curr) );
        curr = end_param + 1;
    }
    add_param(url.substr(curr));
    return _url.substr(0, pos);
}

} // http namespace
} // seastar namespace
