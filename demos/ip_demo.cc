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

#include <seastar/net/arp.hh>
#include <seastar/net/ip.hh>
#include <seastar/net/net.hh>
#include <seastar/core/reactor.hh>
#include <seastar/net/virtio.hh>
#include <seastar/net/native-stack.hh>
#include <seastar/core/aligned_buffer.hh>

using namespace seastar;
using namespace net;

int main(int ac, char** av) {
    native_stack_options opts;

    auto vnet = create_virtio_net_device(opts.virtio_opts, opts.lro);
    vnet->set_local_queue(vnet->init_local_queue(opts, 0));

    interface netif(std::move(vnet));
    ipv4 inet(&netif);
    inet.set_host_address(ipv4_address("192.168.122.2"));
    engine().run();
    return 0;
}



