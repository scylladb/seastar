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
    cmake
    diffutils
    doxygen
    g++
    gcc
    libboost-all-dev
    libc-ares-dev
    libcrypto++-dev
    libfmt-dev
    libgnutls28-dev
    libhwloc-dev
    liblz4-dev
    libnuma-dev
    libpciaccess-dev
    libprotobuf-dev
    libsctp-dev
    libssl-dev
    libtool
    liburing-dev
    libxml2-dev
    libyaml-cpp-dev
    make
    meson
    ninja-build
    openssl
    pkg-config
    protobuf-compiler
    python3
    python3-pyelftools
    python3-yaml
    ragel
    stow
    systemtap-sdt-dev
    valgrind
    xfslibs-dev
)

# seastar doesn't directly depend on these packages. They are
# needed because we want to link seastar statically and pkg-config
# has no way of saying "static seastar, but dynamic transitive
# dependencies". They provide the various .so -> .so.ver symbolic
# links.
transitive=(
    libidn2-devel
    libtool-ltdl-devel
    libunistring-devel
    trousers-devel
)

redhat_packages=(
    boost-devel
    c-ares-devel
    cmake
    diffutils
    doxygen
    fmt-devel
    gcc
    gnutls-devel
    hwloc-devel
    libpciaccess-devel
    libtool
    liburing-devel
    libxml2-devel
    lksctp-tools-devel
    lz4-devel
    make
    meson
    numactl-devel
    openssl
    openssl-devel
    protobuf-compiler
    protobuf-devel
    python3
    python3-pyelftools
    python3-pyyaml
    stow
    systemtap-sdt-devel
    valgrind-devel
    xfsprogs-devel
    yaml-cpp-devel
    "${transitive[@]}"
)

fedora_packages=(
    "${redhat_packages[@]}"
    boost-devel
    fmt-devel
    gcc-c++
    libasan
    libatomic
    libubsan
    ninja-build
    ragel
    valgrind-devel
)

centos7_packages=(
    "${redhat_packages[@]}"
    cmake3
    devtoolset-11-gcc-c++
    devtoolset-11-libasan
    devtoolset-11-libatomic
    devtoolset-11-libubsan
    ninja-build
    ragel
    rh-mongodb36-boost-devel
)

centos8_packages=(
    "${redhat_packages[@]}"
    gcc-toolset-11-gcc
    gcc-toolset-11-gcc-c++
    gcc-toolset-11-libasan-devel
    gcc-toolset-11-libatomic-devel
    gcc-toolset-11-libubsan-devel
    ninja-build
    ragel
)

centos9_packages=(
    "${redhat_packages[@]}"
    gcc-toolset-13-gcc
    gcc-toolset-13-gcc-c++
    gcc-toolset-13-libasan-devel
    gcc-toolset-13-libatomic-devel
    gcc-toolset-13-libubsan-devel
    ninja-build
    ragel
)

# 1) glibc 2.30-3 has sys/sdt.h (systemtap include)
#    some old containers may contain glibc older,
#    so enforce update on that one.
# 2) if problems with signatures, ensure having fresh
#    archlinux-keyring: pacman -Sy archlinux-keyring && pacman -Syyu
# 3) aur installations require having sudo and being
#    a sudoer. makepkg does not work otherwise.
arch_packages=(
    boost
    boost-libs
    c-ares
    cmake
    crypto++
    filesystem
    fmt
    gcc
    glibc
    gnutls
    hwloc
    libpciaccess
    libtool
    liburing
    libxml2
    lksctp-tools
    lz4
    make
    meson
    ninja
    numactl
    openssl
    pkgconf
    protobuf
    python3
    python-pyelftools
    python-yaml
    ragel
    stow
    valgrind
    xfsprogs
    yaml-cpp
)

opensuse_packages=(
    c-ares-devel
    cmake
    hwloc-devel
    libboost_atomic1_66_0
    libboost_atomic1_66_0-devel
    libboost_chrono1_66_0
    libboost_chrono1_66_0-devel
    libboost_date_time1_66_0
    libboost_date_time1_66_0-devel
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
    libgnutls-devel
    libgnutlsxx28
    liblz4-devel
    libnuma-devel
    libtool
    lksctp-tools-devel
    meson
    ninja
    openssl
    openssl-devel
    protobuf-devel
    python3-PyYAML
    ragel
    stow
    xfsprogs-devel
    yaml-cpp-devel
)

case "$ID" in
    ubuntu|debian|pop)
        apt-get install -y "${debian_packages[@]}"
    ;;
    fedora)
        dnf install -y "${fedora_packages[@]}"
    ;;
    rhel|centos|rocky)
        if [ "$VERSION_ID" = "7" ]; then
            yum install -y epel-release centos-release-scl scl-utils
            yum install -y "${centos7_packages[@]}"
        elif [ "${VERSION_ID%%.*}" = "8" ]; then
            dnf install -y epel-release
            dnf install -y "${centos8_packages[@]}"
        elif [ "${VERSION_ID%%.*}" = "9" ]; then
            dnf install -y epel-release
            dnf install -y "${centos9_packages[@]}"
        fi
    ;;
    opensuse-leap)
        zypper install -y "${opensuse_packages[@]}"
    ;;
    arch|manjaro)
        if [ "$EUID" -eq "0" ]; then
            pacman -Sy --needed --noconfirm "${arch_packages[@]}"
        else
            echo "seastar: running without root. Skipping main dependencies (pacman)." 1>&2
        fi
    ;;
    *)
        echo "Your system ($ID) is not supported by this script. Please install dependencies manually."
        exit 1
    ;;
esac
