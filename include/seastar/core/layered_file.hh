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
 * Copyright 2020 ScyllaDB
 */

#pragma once

#include <seastar/core/file.hh>

namespace seastar {

/// \addtogroup fileio-module
/// @{

/// Base class for layered file implementations.
///
/// A layered file implementation implements `file_impl` virtual
/// functions such as dma_read() by forwarding them to another, existing
/// file called the underlying file. This base class simplifies construction
/// of layered files by performing standard tasks such as setting up the
/// file alignment. Actual implementation of the I/O methods is left for the
/// derived class.
class layered_file_impl : public file_impl {
protected:
    file _underlying_file;
public:
    /// Constructs a layered file. This sets up the underlying_file() method
    /// and initializes alignment constants to be the same as the underlying file.
    explicit layered_file_impl(file underlying_file) noexcept
            : _underlying_file(std::move(underlying_file)) {
        _memory_dma_alignment = _underlying_file.memory_dma_alignment();
        _disk_read_dma_alignment = _underlying_file.disk_read_dma_alignment();
        _disk_write_dma_alignment = _underlying_file.disk_write_dma_alignment();
        _disk_overwrite_dma_alignment = _underlying_file.disk_overwrite_dma_alignment();
    }

    /// The underlying file which can be used to back I/O methods.
    file& underlying_file() noexcept {
        return _underlying_file;
    }

    /// The underlying file which can be used to back I/O methods.
    const file& underlying_file() const noexcept {
        return _underlying_file;
    }
};


/// @}


}
