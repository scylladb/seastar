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

#include <seastar/net/net.hh>
#include <seastar/net/virtio.hh>
#include <seastar/net/dpdk.hh>
#include <seastar/util/program-options.hh>

namespace seastar {

struct network_stack_entry;

namespace net {

/// Native stack configuration.
struct native_stack_options : public program_options::option_group {
    program_options::value<std::string> net_config;
    program_options::value<std::string> net_config_file;
    /// \brief Tap device to connect to.
    ///
    /// Default: \p tap0.
    program_options::value<std::string> tap_device;
    /// \brief Static IPv4 address to use.
    ///
    /// Default: \p 192.168.122.2.
    program_options::value<std::string> host_ipv4_addr;
    /// \brief Static IPv4 gateway to use.
    ///
    /// Default: \p 192.168.122.1.
    program_options::value<std::string> gw_ipv4_addr;
    /// \brief Static IPv4 netmask to use.
    ///
    /// Default: \p 255.255.255.0.
    program_options::value<std::string> netmask_ipv4_addr;
    /// \brief Default size of the UDPv4 per-channel packet queue.
    ///
    /// Default: \ref ipv4_udp::default_queue_size.
    program_options::value<int> udpv4_queue_size;
    /// \brief Use DHCP discovery.
    ///
    /// Default: \p true.
    program_options::value<bool> dhcp;
    /// \brief Weighing of a hardware network queue relative to a software queue.
    ///
    /// Values:
    /// * 0.0: no work
    /// * 1.0: equal share
    ///
    /// Default: 1.0.
    program_options::value<float> hw_queue_weight;
    /// \brief Use DPDK PMD drivers.
    ///
    /// \note Unused when seastar is compiled without DPDK support.
    program_options::value<> dpdk_pmd;
    /// \brief Enable LRO (on/off).
    ///
    /// Default: \p on.
    program_options::value<std::string> lro;

    /// Virtio configuration.
    virtio_options virtio_opts;
    /// DPDK configuration.
    ///
    /// \note Unused when seastar is compiled without DPDK support.
    dpdk_options dpdk_opts;

    /// \cond internal
    bool _hugepages;

    native_stack_options();
    /// \endcond
};

void create_native_stack(const native_stack_options& opts, std::shared_ptr<device> dev);
network_stack_entry register_native_stack();

}

}
