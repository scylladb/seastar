#!/bin/bash
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

. /etc/os-release

if [ "$ID" = "ubuntu" ] || [ "$ID" = "debian" ]; then
    if [ "$VERSION_ID" = "14.04" ]; then
        if [ ! -f /usr/bin/add-apt-repository ]; then
            apt-get -y install software-properties-common
        fi

        add-apt-repository -y ppa:ubuntu-toolchain-r/test
        apt-get -y update
    fi
    apt-get install -y libaio-dev ninja-build ragel libhwloc-dev libnuma-dev libpciaccess-dev libcrypto++-dev libboost-all-dev libxml2-dev xfslibs-dev libgnutls28-dev liblz4-dev libsctp-dev gcc make libprotobuf-dev protobuf-compiler python3 libunwind8-dev systemtap-sdt-dev libtool cmake
    if [ "$ID" = "ubuntu" ]; then
        apt-get install -y g++-5
        echo "g++-5 is installed for Seastar. To build Seastar with g++-5, specify '--compiler=g++-5' on configure.py"
    else # debian
        apt-get install -y g++
    fi
elif [ "$ID" = "centos" ] || [ "$ID" = "fedora" ]; then
    if [ "$ID" = "centos" ]; then
        yum install -y epel-release
        curl -o /etc/yum.repos.d/scylla-1.2.repo http://downloads.scylladb.com/rpm/centos/scylla-1.2.repo
    fi
    yum install -y libaio-devel hwloc-devel numactl-devel libpciaccess-devel cryptopp-devel libxml2-devel xfsprogs-devel gnutls-devel lksctp-tools-devel lz4-devel gcc make protobuf-devel protobuf-compiler libunwind-devel systemtap-sdt-devel libtool cmake
    if [ "$ID" = "fedora" ]; then
        dnf install -y gcc-c++ ninja-build ragel boost-devel libubsan libasan
    else # centos
        yum install -y scylla-binutils scylla-gcc-c++ scylla-ninja-build scylla-ragel scylla-boost-devel scylla-libubsan scylla-libasan scylla-libstdc++-static python34
        echo "g++-5 is installed for Seastar. To build Seastar with g++-5, specify '--compiler=/opt/scylladb/bin/g++ --static-stdc++' on configure.py"
        echo "Before running ninja-build, execute following command: . /etc/profile.d/scylla.sh"
    fi
elif [ "$ID" = "arch" ]; then
    pacman -Sy --needed gcc ninja ragel boost boost-libs libaio hwloc numactl libpciaccess crypto++ libxml2 xfsprogs gnutls lksctp-tools lz4 make protobuf libunwind systemtap libtool cmake
fi
