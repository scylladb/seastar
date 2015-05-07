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
 * Copyright (C) 2015 Cloudius Systems, Ltd.
 */

#include <string>
#include <memory>
#include "core/future.hh"
#include "nat-adapter.hh"
#include "native-stack.hh"
#include "dpdk.hh"
#include "virtio.hh"
#include "proxy.hh"
#include "ip.hh"
#include "tcp.hh"
#include <sys/ioctl.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <net/route.h>

namespace net {

void create_nat_adapter_device(boost::program_options::variables_map opts, std::shared_ptr<device> seastar_dev)
{
     create_device(opts, false).then([opts, seastar_dev = std::move(seastar_dev)] (std::shared_ptr<device> nat_adapter_dev) {
        for (unsigned i = 0; i < smp::count; i++) {
            smp::submit_to(i, [opts, nat_adapter_dev, seastar_dev] {
                create_nat_adapter(opts, nat_adapter_dev, seastar_dev);
            });
        }
    });
}

void create_nat_adapter(boost::program_options::variables_map opts, std::shared_ptr<device> nat_adapter_dev, std::shared_ptr<device> seastar_dev) {
    nat_adapter::ready_promise.set_value(lw_shared_ptr<nat_adapter>(make_lw_shared<nat_adapter>(opts, nat_adapter_dev, seastar_dev)));
}

nat_adapter_interface::nat_adapter_interface(std::shared_ptr<device> nat_adapter_dev, std::shared_ptr<device> seastar_dev)
    : _nat_adapter_dev(nat_adapter_dev)
    , _seastar_dev(seastar_dev)
    , _rx(_nat_adapter_dev->receive([this] (packet p) { return receive(std::move(p)); }))
{
    // Received from DPDK interface, forward to TAP interface
    nat_adapter_dev->local_queue().register_packet_provider([this] () mutable {
            std::experimental::optional<packet> p;
            if (!_txq.empty()) {
                p = std::move(_txq.front());
                _txq.pop_front();
            }
            return p;
        });
    // Received from TAP interface, forward to DPDK interface
    seastar_dev->local_queue().register_packet_provider([this] () mutable {
            std::experimental::optional<packet> p;
            if (!_rxq.empty()) {
                p = std::move(_rxq.front());
                _rxq.pop_front();
            }
            return p;
        });
}

future<> nat_adapter_interface::receive(packet p) {
    _rxq.push_back(std::move(p));
    return make_ready_future<>();
}

void nat_adapter_interface::send(packet p) {
    _txq.push_back(std::move(p));
}

thread_local promise<lw_shared_ptr<nat_adapter>> nat_adapter::ready_promise;

future<lw_shared_ptr<nat_adapter>> nat_adapter::create(boost::program_options::variables_map opts, std::shared_ptr<device> seastar_dev) {
    if (engine().cpu_id() == 0) {
            create_nat_adapter_device(opts, seastar_dev);
    }
    return ready_promise.get_future();
}

nat_adapter::nat_adapter(boost::program_options::variables_map opts, std::shared_ptr<device> nat_adapter_dev, std::shared_ptr<device> seastar_dev)
    : _netif(nat_adapter_dev, seastar_dev),
    _name(opts["tap-device"].as<std::string>()) {}

void nat_adapter::down()
{
    ifreq ifr = {};
    auto fd = file_desc::socket(AF_INET, SOCK_STREAM, 0);
    strcpy(ifr.ifr_ifrn.ifrn_name, _name.c_str());
    fd.ioctl(SIOCGIFFLAGS, ifr);
    ifr.ifr_flags &= ~IFF_UP;
    fd.ioctl(SIOCSIFFLAGS, ifr);
}

void nat_adapter::up()
{
    ifreq ifr = {};
    auto fd = file_desc::socket(AF_INET, SOCK_STREAM, 0);
    strcpy(ifr.ifr_ifrn.ifrn_name, _name.c_str());
    fd.ioctl(SIOCGIFFLAGS, ifr);
    ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
    fd.ioctl(SIOCSIFFLAGS, ifr);
}

void nat_adapter::set_hw_address(ethernet_address addr)
{
    down();
    ifreq ifr = {};
    auto fd = file_desc::socket(AF_INET, SOCK_STREAM, 0);
    strcpy(ifr.ifr_ifrn.ifrn_name, _name.c_str());
    ifr.ifr_hwaddr.sa_family = ARPHRD_ETHER;
    for (int i = 0; i < 6; i++)
        ifr.ifr_hwaddr.sa_data[i] = addr.mac[i];
    fd.ioctl(SIOCSIFHWADDR, ifr);
    up();
}

void nat_adapter::send(packet p)
{
    _netif.send(std::move(p));
}

}
