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

# os-release may be missing in container environment by default.
if [ -f "/etc/os-release" ]; then
    . /etc/os-release
elif [ -f "/etc/arch-release" ]; then
    export ID=arch
else
    echo "/etc/os-release missing."
    exit 1
fi

debian_packages=(
    ninja-build
    ragel
    libhwloc-dev
    libnuma-dev
    libpciaccess-dev
    libcrypto++-dev
    libboost-all-dev
    libxml2-dev
    xfslibs-dev
    libgnutls28-dev
    liblz4-dev
    libsctp-dev
    gcc
    make
    libprotobuf-dev
    protobuf-compiler
    python3
    systemtap-sdt-dev
    libtool
    cmake
    libyaml-cpp-dev
    libc-ares-dev
    stow
    g++
    libfmt-dev
    diffutils
    valgrind
)

# seastar doesn't directly depend on these packages. They are
# needed because we want to link seastar statically and pkg-config
# has no way of saying "static seastar, but dynamic transitive
# dependencies". They provide the various .so -> .so.ver symbolic
# links.
transitive=(libtool-ltdl-devel trousers-devel libidn2-devel libunistring-devel)

redhat_packages=(
    hwloc-devel
    numactl-devel
    libpciaccess-devel
    cryptopp-devel
    libxml2-devel
    xfsprogs-devel
    gnutls-devel
    lksctp-tools-devel
    lz4-devel
    gcc
    make
    protobuf-devel
    protobuf-compiler
    systemtap-sdt-devel
    libtool
    cmake
    yaml-cpp-devel
    c-ares-devel
    stow
    diffutils
    "${transitive[@]}"
)

fedora_packages=(
    "${redhat_packages[@]}"
    gcc-c++
    ninja-build
    ragel
    boost-devel
    fmt-devel
    libubsan
    libasan
    libatomic
    valgrind-devel
)

centos_packages=(
    "${redhat_packages[@]}"
    ninja-build
    ragel
    rh-mongodb36-boost-devel
    devtoolset-8-gcc-c++
    devtoolset-8-libubsan
    devtoolset-8-libasan
    devtoolset-8-libatomic
)

# 1) glibc 2.30-3 has sys/sdt.h (systemtap include)
#    some old containers may contain glibc older,
#    so enforce update on that one.
# 2) if problems with signatures, ensure having fresh
#    archlinux-keyring: pacman -Sy archlinux-keyring && pacman -Syyu
# 3) aur installations require having sudo and being
#    a sudoer. makepkg does not work otherwise.
arch_packages=(
    gcc
    ninja
    ragel
    boost
    boost-libs
    hwloc
    numactl
    libpciaccess
    crypto++
    libxml2
    xfsprogs
    gnutls
    lksctp-tools
    lz4
    make
    protobuf
    libtool
    cmake
    yaml-cpp
    stow
    c-ares
    pkgconf
    fmt
    python3
    glibc
    filesystem
    valgrind
)

opensuse_packages=(
    c-ares-devel
    cmake
    hwloc-devel
    libboost_filesystem1_66_0
    libboost_filesystem1_66_0-devel
    libboost_program_options1_66_0
    libboost_program_options1_66_0-devel
    libboost_system1_66_0
    libboost_system1_66_0-devel
    libboost_test1_66_0
    libboost_test1_66_0-devel
    libboost_thread1_66_0
    libboost_thread1_66_0-devel
    libcryptopp-devel
    libboost_atomic1_66_0
    libboost_atomic1_66_0-devel
    libboost_date_time1_66_0
    libboost_date_time1_66_0-devel
    libboost_chrono1_66_0
    libboost_chrono1_66_0-devel
    libgnutls-devel
    libgnutlsxx28
    liblz4-devel
    libnuma-devel
    lksctp-tools-devel
    ninja protobuf-devel
    ragel
    xfsprogs-devel
    yaml-cpp-devel
    libtool
    stow
)

if [ "$ID" = "ubuntu" ] || [ "$ID" = "debian" ]; then
    apt-get install -y "${debian_packages[@]}"
elif [ "$ID" = "centos" ] || [ "$ID" = "fedora" ]; then
    if [ "$ID" = "fedora" ]; then
        dnf install -y "${fedora_packages[@]}"
    else # centos
        yum install -y epel-release centos-release-scl scl-utils
        yum install -y "${centos_packages[@]}" 
    fi
elif [ "$ID" = "arch" ]; then
    # main
    if [ "$EUID" -eq "0" ]; then
        pacman -Sy --needed --noconfirm "${arch_packages[@]}"
    else
        echo "seastar: running without root. Skipping main dependencies (pacman)." 1>&2
    fi
elif [ "$ID" = "opensuse-leap" ]; then
    zypper install -y "${opensuse_packages[@]}"
else
    echo "Your system ($ID) is not supported by this script. Please install dependencies manually."
    exit 1
fi
