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
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 */

#pragma once

#include <memory>
#include <seastar/net/net.hh>
#include <seastar/core/sstring.hh>
#include <seastar/util/program-options.hh>

namespace seastar {

namespace net {

/// Virtio configuration.
struct virtio_options : public program_options::option_group {
    /// \brief Enable event-index feature (on / off).
    ///
    /// Default: \p on.
    program_options::value<std::string> event_index;
    /// \brief Enable checksum offload feature (on / off).
    ///
    /// Default: \p on.
    program_options::value<std::string> csum_offload;
    /// \brief Enable TCP segment offload feature (on / off).
    ///
    /// Default: \p on.
    program_options::value<std::string> tso;
    /// \brief Enable UDP fragmentation offload feature (on / off).
    ///
    /// Default: \p on.
    program_options::value<std::string> ufo;
    /// \brief Virtio ring size (must be power-of-two).
    ///
    /// Default: 256.
    program_options::value<unsigned> virtio_ring_size;

    /// \cond internal
    virtio_options(program_options::option_group* parent_group);
    /// \endcond
};

}

/// \cond internal
std::unique_ptr<net::device> create_virtio_net_device(const net::virtio_options& opts, const program_options::value<std::string>& lro);
/// \endcond

}
