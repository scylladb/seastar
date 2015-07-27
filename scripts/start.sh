#!/bin/sh -e
#
# This file is open source software, licensed to you under the terms
# of the Apache License, Version 2.0 (the "License").  See the NOTICE file
# distributed with this work for additional information regarding copyright
# ownership.  You may not use this file except in compliance with the License.
#
# You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.
#

if [ $# -lt 1 ]; then
    echo "usage: $0 [config]"
    exit 1
fi

. ./$1

if [ "$mode" = "nat" ]; then
    if [ "$netconfig" = "seastar" ]; then
        if [ "`ip link show $bridge`" = "" ]; then
            killall dhclient || true
            eth_mac=`ip link show $eth |grep link|awk '{print $2}'`
            eth_ip=`ip addr show $eth |grep 'inet '|awk '{print $2}'`
            ip addr del $eth_ip dev $eth
            ip link add name $bridge type bridge
            ip link set $bridge address $eth_mac
            ip link set dev $eth master $bridge
            dhclient $bridge
        fi
    fi
fi

if [ "$mode" = "dpdk" ] || [ "$mode" = "nat" ]; then
    modprobe uio
    if [ "`lsmod|grep igb_uio`" = "" ]; then
        insmod $dpdk_build/kmod/igb_uio.ko
    fi
    $dpdk_src/tools/dpdk_nic_bind.py --force --bind=igb_uio $eth
    mkdir -p /mnt/huge
    mount -t hugetlbfs nodev /mnt/huge
    echo $nr_hugepages > /sys/devices/system/node/node0/hugepages/hugepages-2048kB/nr_hugepages
    args="$args --dpdk-pmd"
fi

if [ "$mode" = "nat" ]; then
    ip tuntap del mode tap dev $tap
    ip tuntap add mode tap dev $tap one_queue vnet_hdr
    ip link set dev $tap up
    ip link set dev $tap master $bridge
    modprobe vhost-net
    chown $user.$user /dev/vhost-net
    args="$args --nat-adapter"
fi

if [ "$mode" = "virtio" ]; then
    user=`whoami`
    sudo ip tuntap del mode tap dev $tap
    sudo ip tuntap add mode tap dev $tap user $user one_queue vnet_hdr
    sudo ip link set dev $tap up
    sudo ip link set dev $tap master $bridge
    sudo modprobe vhost-net
    sudo chown $user.$user /dev/vhost-net
fi

export LD_LIBRARY_PATH=$dpdk_target/lib
$program $args
