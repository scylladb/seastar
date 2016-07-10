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
 * Copyright (C) 2016 Scylladb, Ltd.
 */

#include "core/sstring.hh"
#include "rpc/rpc_types.hh"
#include <lz4.h>

namespace rpc {
    class lz4_compressor : public compressor {
    public:
        ~lz4_compressor() {}
        // compress data, leaving head_space empty in returned buffer
        sstring compress(size_t head_space, sstring data) override;
        // decompress data
        temporary_buffer<char> decompress(temporary_buffer<char> data) override;
    };
}
