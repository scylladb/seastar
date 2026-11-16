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

#pragma once

#include <seastar/core/iostream.hh>

namespace seastar::http::internal {

// Output streams that compress what is written to them and write the result
// to \c out, for use as an HTTP content coding ("Content-Encoding").
//
// Closing the returned stream ends the compressed stream and closes \c out.
//
// Compression runs on the reactor thread. The streams compress at the fastest
// level and never compress more than a few tens of kilobytes without giving
// the reactor a chance to preempt, whatever the size of a single write.
//
// Each function is available only when Seastar was built with the library.

#ifdef SEASTAR_HAVE_ZLIB
output_stream<char> make_gzip_output_stream(output_stream<char>&& out);
#endif

#ifdef SEASTAR_HAVE_ZSTD
output_stream<char> make_zstd_output_stream(output_stream<char>&& out);
#endif

}
