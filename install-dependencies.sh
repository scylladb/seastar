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
    apt-get install -y ninja-build ragel libhwloc-dev libnuma-dev libpciaccess-dev libcrypto++-dev libboost-all-dev libxml2-dev xfslibs-dev libgnutls28-dev liblz4-dev libsctp-dev gcc make libprotobuf-dev protobuf-compiler python3 systemtap-sdt-dev libtool cmake libyaml-cpp-dev libc-ares-dev stow
    if [ "$ID" = "ubuntu" ]; then
        apt-get install -y g++-5
        echo "g++-5 is installed for Seastar. To build Seastar with g++-5, specify '--compiler=g++-5' on configure.py"
    else # debian
        apt-get install -y g++
    fi
elif [ "$ID" = "centos" ] || [ "$ID" = "fedora" ]; then
    if [ "$ID" = "centos" ]; then
        yum install -y epel-release
        cat > /etc/yum.repos.d/scylladb-copr.repo <<EOF
[scylladb-scylla-3rdparty]
name=Copr repo for scylla-3rdparty owned by scylladb
baseurl=https://copr-be.cloud.fedoraproject.org/results/scylladb/scylla-3rdparty/epel-7-x86_64/
type=rpm-md
skip_if_unavailable=True
gpgcheck=1
gpgkey=https://copr-be.cloud.fedoraproject.org/results/scylladb/scylla-3rdparty/pubkey.gpg
repo_gpgcheck=0
enabled=1
enabled_metadata=1
EOF
    fi
    yum install -y hwloc-devel numactl-devel libpciaccess-devel cryptopp-devel libxml2-devel xfsprogs-devel gnutls-devel lksctp-tools-devel lz4-devel gcc make protobuf-devel protobuf-compiler systemtap-sdt-devel libtool cmake yaml-cpp-devel c-ares-devel stow
    if [ "$ID" = "fedora" ]; then
        dnf install -y gcc-c++ ninja-build ragel boost-devel libubsan libasan libatomic
    else # centos
        yum install -y scylla-binutils scylla-gcc73-c++ ninja-build ragel-devel scylla-boost163-devel scylla-libubsan73-static scylla-libasan73-static scylla-libstdc++73-static scylla-libatomic73-static python34
        echo "g++-7.3 is installed for Seastar. To build Seastar with g++-7.3, specify '--compiler=/opt/scylladb/bin/g++ --static-stdc++' on configure.py"
        echo "Before running ninja-build, execute following command: . /etc/profile.d/scylla.sh"
    fi
elif [ "$ID" = "arch" -o "$ID_LIKE" = "arch" ]; then
    pacman -Sy --needed gcc ninja ragel boost boost-libs hwloc numactl libpciaccess crypto++ libxml2 xfsprogs gnutls lksctp-tools lz4 make protobuf systemtap libtool cmake yaml-cpp stow
elif [ "$ID" = "opensuse-leap" ]; then
    zypper install -y c-ares-devel cmake hwloc-devel libboost_filesystem1_66_0 libboost_filesystem1_66_0-devel libboost_program_options1_66_0 libboost_program_options1_66_0-devel libboost_system1_66_0 libboost_system1_66_0-devel libboost_test1_66_0 libboost_test1_66_0-devel libboost_thread1_66_0 libboost_thread1_66_0-devel libcryptopp-devel libboost_atomic1_66_0 libboost_atomic1_66_0-devel libboost_date_time1_66_0 libboost_date_time1_66_0-devel libboost_chrono1_66_0 libboost_chrono1_66_0-devel libgnutls-devel libgnutlsxx28 liblz4-devel libnuma-devel lksctp-tools-devel ninja protobuf-devel ragel xfsprogs-devel yaml-cpp-devel libtool stow
else
    echo "Your system ($ID) is not supported by this script. Please install dependencies manually."
    exit 1
fi
