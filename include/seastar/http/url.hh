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

#include <seastar/core/sstring.hh>

namespace seastar {

namespace http {
namespace internal {

bool url_decode(const std::string_view& in, sstring& out);

/**
 * Makes a percent-encoded string out of the given parameter
 *
 * Note, that it does NOT parse and encode a URL correctly handling
 * all the delimeters in arguments. It's up to the caller to split
 * the URL into path and arguments (both names and values) and use
 * this helper to encode individual strings
 */
sstring url_encode(const std::string_view& in);

} // internal namespace
} // http namespace

} // seastar namespace
