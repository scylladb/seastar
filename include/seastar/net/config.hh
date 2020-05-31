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
 * Copyright 2017 Marek Waszkiewicz ( marek.waszkiewicz77@gmail.com )
 */

#pragma once

#include <seastar/util/std-compat.hh>
#include <istream>
#include <string>
#include <unordered_map>

namespace seastar {
namespace net {

    struct ipv4_config {
        std::string ip;
        std::string netmask;
        std::string gateway;
        bool dhcp{ false };
    };

    struct hw_config {
        std::string pci_address;
        std::optional<unsigned> port_index;
        bool lro{ true };
        bool tso{ true };
        bool ufo{ true };
        bool hw_fc{ true };
        bool event_index{ true };
        bool csum_offload{ true };
        std::optional<unsigned> ring_size;
    };

    struct device_config {
        ipv4_config ip_cfg;
        hw_config hw_cfg;
    };

    std::unordered_map<std::string, device_config> parse_config(std::istream& input);

    class config_exception : public std::runtime_error {
    public:
        config_exception(const std::string& msg)
            : std::runtime_error(msg) {
        }
    };
}
}
