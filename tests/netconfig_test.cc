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

#define BOOST_TEST_MODULE core

#include "net/config.hh"
#include <boost/test/included/unit_test.hpp>
#include <exception>
#include <sstream>

using namespace seastar::net;

BOOST_AUTO_TEST_CASE(test_valid_config_with_pci_address) {
    std::stringstream ss;
    ss << "{eth0: {pci-address: 0000:06:00.0, ip: 192.168.100.10, gateway: 192.168.100.1, netmask: "
          "255.255.255.0 } , eth1: {pci-address: 0000:06:00.1, dhcp: true } }";
    auto device_configs = parse_config(ss);

    // eth0 tests
    BOOST_REQUIRE(device_configs.find("eth0") != device_configs.end());
    BOOST_REQUIRE_EQUAL(device_configs.at("eth0").hw_cfg.pci_address, "0000:06:00.0");
    BOOST_REQUIRE_EQUAL(device_configs.at("eth0").ip_cfg.dhcp, false);
    BOOST_REQUIRE_EQUAL(device_configs.at("eth0").ip_cfg.ip, "192.168.100.10");
    BOOST_REQUIRE_EQUAL(device_configs.at("eth0").ip_cfg.gateway, "192.168.100.1");
    BOOST_REQUIRE_EQUAL(device_configs.at("eth0").ip_cfg.netmask, "255.255.255.0");

    // eth1 tests
    BOOST_REQUIRE(device_configs.find("eth1") != device_configs.end());
    BOOST_REQUIRE_EQUAL(device_configs.at("eth1").hw_cfg.pci_address, "0000:06:00.1");
    BOOST_REQUIRE_EQUAL(device_configs.at("eth1").ip_cfg.dhcp, true);
    BOOST_REQUIRE_EQUAL(device_configs.at("eth1").ip_cfg.ip, "");
    BOOST_REQUIRE_EQUAL(device_configs.at("eth1").ip_cfg.gateway, "");
    BOOST_REQUIRE_EQUAL(device_configs.at("eth1").ip_cfg.netmask, "");
}

BOOST_AUTO_TEST_CASE(test_valid_config_with_port_index) {
    std::stringstream ss;
    ss << "{eth0: {port-index: 0, ip: 192.168.100.10, gateway: 192.168.100.1, netmask: "
          "255.255.255.0 } , eth1: {port-index: 1, dhcp: true } }";
    auto device_configs = parse_config(ss);

    // eth0 tests
    BOOST_REQUIRE(device_configs.find("eth0") != device_configs.end());
    BOOST_REQUIRE_EQUAL(*device_configs.at("eth0").hw_cfg.port_index, 0);
    BOOST_REQUIRE_EQUAL(device_configs.at("eth0").ip_cfg.dhcp, false);
    BOOST_REQUIRE_EQUAL(device_configs.at("eth0").ip_cfg.ip, "192.168.100.10");
    BOOST_REQUIRE_EQUAL(device_configs.at("eth0").ip_cfg.gateway, "192.168.100.1");
    BOOST_REQUIRE_EQUAL(device_configs.at("eth0").ip_cfg.netmask, "255.255.255.0");

    // eth1 tests
    BOOST_REQUIRE(device_configs.find("eth1") != device_configs.end());
    BOOST_REQUIRE_EQUAL(*device_configs.at("eth1").hw_cfg.port_index, 1);
    BOOST_REQUIRE_EQUAL(device_configs.at("eth1").ip_cfg.dhcp, true);
    BOOST_REQUIRE_EQUAL(device_configs.at("eth1").ip_cfg.ip, "");
    BOOST_REQUIRE_EQUAL(device_configs.at("eth1").ip_cfg.gateway, "");
    BOOST_REQUIRE_EQUAL(device_configs.at("eth1").ip_cfg.netmask, "");
}

BOOST_AUTO_TEST_CASE(test_valid_config_single_device) {
    std::stringstream ss;
    ss << "eth0: {pci-address: 0000:06:00.0, ip: 192.168.100.10, gateway: 192.168.100.1, netmask: "
          "255.255.255.0 }";
    auto device_configs = parse_config(ss);

    // eth0 tests
    BOOST_REQUIRE(device_configs.find("eth0") != device_configs.end());
    BOOST_REQUIRE_EQUAL(device_configs.at("eth0").hw_cfg.pci_address, "0000:06:00.0");
    BOOST_REQUIRE_EQUAL(device_configs.at("eth0").ip_cfg.dhcp, false);
    BOOST_REQUIRE_EQUAL(device_configs.at("eth0").ip_cfg.ip, "192.168.100.10");
    BOOST_REQUIRE_EQUAL(device_configs.at("eth0").ip_cfg.gateway, "192.168.100.1");
    BOOST_REQUIRE_EQUAL(device_configs.at("eth0").ip_cfg.netmask, "255.255.255.0");
}

BOOST_AUTO_TEST_CASE(test_unsupported_key) {
    std::stringstream ss;
    ss << "{eth0: { some_not_supported_tag: xxx, pci-address: 0000:06:00.0, ip: 192.168.100.10, "
          "gateway: 192.168.100.1, netmask: 255.255.255.0 } , eth1: {pci-address: 0000:06:00.1, "
          "dhcp: true } }";

    BOOST_REQUIRE_THROW(parse_config(ss), config_exception);
}

BOOST_AUTO_TEST_CASE(test_bad_yaml_syntax_if_thrown) {
    std::stringstream ss;
    ss << "some bad: [ yaml syntax }";
    BOOST_REQUIRE_THROW(parse_config(ss), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(test_pci_address_and_port_index_if_thrown) {
    std::stringstream ss;
    ss << "{eth0: {pci-address: 0000:06:00.0, port-index: 0, ip: 192.168.100.10, gateway: "
          "192.168.100.1, netmask: 255.255.255.0 } , eth1: {pci-address: 0000:06:00.1, dhcp: true} "
          "}";
    BOOST_REQUIRE_THROW(parse_config(ss), config_exception);
}

BOOST_AUTO_TEST_CASE(test_dhcp_and_ip_if_thrown) {
    std::stringstream ss;
    ss << "{eth0: {pci-address: 0000:06:00.0, ip: 192.168.100.10, gateway: 192.168.100.1, netmask: "
          "255.255.255.0, dhcp: true } , eth1: {pci-address: 0000:06:00.1, dhcp: true} }";
    BOOST_REQUIRE_THROW(parse_config(ss), config_exception);
}

BOOST_AUTO_TEST_CASE(test_ip_missing_if_thrown) {
    std::stringstream ss;
    ss << "{eth0: {pci-address: 0000:06:00.0, gateway: 192.168.100.1, netmask: 255.255.255.0 } , "
          "eth1: {pci-address: 0000:06:00.1, dhcp: true} }";
    BOOST_REQUIRE_THROW(parse_config(ss), config_exception);
}
