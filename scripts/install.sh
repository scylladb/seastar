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

if [ "$mode" != "nat" ]; then
    exit 0
fi

if [ "$netconfig" = "nmcli" ]; then
    eth_conn_name=`./get_conn_name.py $eth`
    if [ "$eth_conn_name" = "" ]; then
        echo "Cannot detect $eth connection name"
        exit 1
    fi
    nmcli c add type bridge ifname $bridge
    nmcli c mod bridge-$bridge bridge.stp no
    nmcli c add type bridge-slave ifname $eth master bridge-$bridge
    nmcli c del "$eth_conn_name"
fi
echo "net.ipv4.ip_local_port_range= $local_port_start $local_port_end" >> /etc/sysctl.conf
sysctl -p
