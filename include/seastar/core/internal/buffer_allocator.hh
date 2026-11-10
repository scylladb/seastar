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
 * Copyright (C) 2020 ScyllaDB
 */

#pragma once

namespace seastar {

template <typename CharType>
class temporary_buffer;

namespace internal {

// Internal interface for allocating buffers for reads. Used to decouple
// allocation strategies (where to allocate from, and what sizes) from the
// point where allocation happens, to make it as late as possible.
class buffer_allocator {
public:
    virtual ~buffer_allocator() = default;
    virtual temporary_buffer<char> allocate_buffer() = 0;
    /// Called with the buffer that a successful recv_some() filled, trimmed
    /// to the number of bytes received (empty at end of stream), before the
    /// future of the receive is made ready. This lets the owner of the
    /// allocator act on what was received without a continuation of its own.
    /// read_some() does not call it.
    ///
    /// It is called from wherever the backend completes the receive:
    /// synchronously inside recv_some() when data is already available, in a
    /// continuation of the poll for readability, or while the reactor reaps
    /// I/O completions, which happens outside of any task, and also inside an
    /// unrelated task that needs a free submission queue entry. So it must
    /// not rely on current_scheduling_group(), and the allocator must stay
    /// alive until every receive that uses it completes. If it throws, the
    /// receive fails with the exception.
    virtual void received(temporary_buffer<char>&) {}
};


}

}
