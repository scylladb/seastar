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

#include "net/config.hh"
#include "core/print.hh"
#include <boost/algorithm/cxx11/all_of.hpp>
#include <boost/algorithm/cxx11/none_of.hpp>
#include <boost/next_prior.hpp>
#include <yaml-cpp/yaml.h>
#include <algorithm>
#include <istream>
#include <unordered_map>
#include <string>

using namespace boost::algorithm;

namespace seastar {
namespace net {

    // list of supported config keys
    std::string config_keys[]{ "pci-address", "port-index", "ip", "gateway", "netmask", "dhcp", "lro", "tso", "ufo", "hw-fc", "event-index", "csum-offload","ring-size" };

    std::unordered_map<std::string, device_config>
    parse_config(std::istream& input) {
        std::unordered_map<std::string, device_config> device_configs;

        YAML::Node doc = YAML::Load(input);
        for (auto&& item : doc) {
            device_configs[item.first.as<std::string>()] = item.second.as<device_config>();
        }

        bool port_index_used = false;
        bool pci_address_used = false;

        for (auto&& item : device_configs) {

            if (item.second.hw_cfg.port_index) {
                port_index_used = true;
            }

            if (!item.second.hw_cfg.pci_address.empty()) {
                pci_address_used = true;
            }

            if (port_index_used && pci_address_used) {
                throw config_exception("port_index and pci_address cannot be used together");
            }
        }

        // check if all of ip,gw,nm are specified when dhcp is off
        if (all_of(device_configs, [](std::pair<std::string, device_config> p) {
                return !(!p.second.ip_cfg.dhcp
                    && (!p.second.ip_cfg.ip.empty() && !p.second.ip_cfg.gateway.empty()
                           && !p.second.ip_cfg.netmask.empty()));
            })) {
            throw config_exception(
                "when dhcp is off then all of ip, gateway, netmask has to be specified");
        }

        // check if dhcp is not used when ip/gw/nm are specified
        if (all_of(device_configs, [](std::pair<std::string, device_config> p) {
                return p.second.ip_cfg.dhcp
                    && !(p.second.ip_cfg.ip.empty() || p.second.ip_cfg.gateway.empty()
                           || p.second.ip_cfg.netmask.empty());
            })) {
            throw config_exception("dhcp and ip cannot be used together");
        }
        return device_configs;
    }
}
}

/// YAML parsing functions
namespace YAML {
template <>
struct convert<seastar::net::device_config> {
    static bool
    decode(const Node& node, seastar::net::device_config& dev_cfg) {
        // test for unsupported key

        for (auto&& item : node) {
            if (none_of(seastar::net::config_keys, [&item](std::string s) {
                    return s == item.first.as<std::string>();
                })) {
                throw seastar::net::config_exception(
                    seastar::sprint("unsuppoted key %s", item.first.as<std::string>()));
            }
        }

        if (node["pci-address"]) {
            dev_cfg.hw_cfg.pci_address = node["pci-address"].as<std::string>();
        }

        if (node["port-index"]) {
            dev_cfg.hw_cfg.port_index = node["port-index"].as<unsigned>();
        }

        if (node["lro"]) {
            dev_cfg.hw_cfg.lro = node["lro"].as<bool>();
        }

        if (node["tso"]) {
            dev_cfg.hw_cfg.tso = node["tso"].as<bool>();
        }

        if (node["ufo"]) {
            dev_cfg.hw_cfg.ufo = node["ufo"].as<bool>();
        }

        if (node["hw-fc"]) {
            dev_cfg.hw_cfg.hw_fc = node["hw-fc"].as<bool>();
        }

        if (node["event-index"]) {
            dev_cfg.hw_cfg.event_index = node["event-index"].as<bool>();
        }

        if (node["ring-size"]) {
            dev_cfg.hw_cfg.ring_size = node["ring-size"].as<unsigned>();
        }

        if (node["ip"]) {
            dev_cfg.ip_cfg.ip = node["ip"].as<std::string>();
        }

        if (node["netmask"]) {
            dev_cfg.ip_cfg.netmask = node["netmask"].as<std::string>();
        }

        if (node["gateway"]) {
            dev_cfg.ip_cfg.gateway = node["gateway"].as<std::string>();
        }

        if (node["dhcp"]) {
            dev_cfg.ip_cfg.dhcp = node["dhcp"].as<bool>();
        }

        return true;
    }
};
}
