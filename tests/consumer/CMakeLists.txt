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
# Copyright (C) 2026 ScyllaDB
#
# A standalone project that builds one small application against an *installed*
# Seastar, once through each channel Seastar is published on: the CMake package
# (find_package (Seastar), Seastar::seastar) and pkg-config (seastar.pc).
#
# Neither channel is reachable from Seastar's own build tree. Its targets link
# the seastar target directly and never see SeastarConfig.cmake or seastar.pc,
# so what those two files say about dependencies, flags and definitions is
# whatever it was last edited to say. This project is what checks it.
#
# It is not part of Seastar's build. Install Seastar somewhere, then:
#
#   cmake -G Ninja -S tests/consumer -B /tmp/consumer \
#         -DCMAKE_PREFIX_PATH=<install prefix> -DCMAKE_CXX_STANDARD=<same as Seastar>
#   cmake --build /tmp/consumer
#
# with PKG_CONFIG_PATH naming the directory the installed seastar.pc landed in.
# Building is the test: a consumer that cannot find the headers, is handed the
# wrong flags, or is left with an unresolved dependency fails here.

cmake_minimum_required (VERSION 3.28)

project (SeastarConsumer
  LANGUAGES CXX)

# Seastar's headers need at least C++20, and its installed package warns if the
# standard differs from the one it was built with; pass -DCMAKE_CXX_STANDARD to
# match.
if (NOT CMAKE_CXX_STANDARD)
  set (CMAKE_CXX_STANDARD 20)
endif ()
set (CMAKE_CXX_STANDARD_REQUIRED ON)
# Seastar builds with GNU extensions on. seastar.pc names a standard of its
# own, which lands after this one and wins for that channel.
set (CMAKE_CXX_EXTENSIONS ON)

#
# Channel 1: the CMake package.
#

find_package (Seastar REQUIRED)

add_executable (consumer_cmake consumer.cc)
target_link_libraries (consumer_cmake
  PRIVATE
    Seastar::seastar)

#
# Channel 2: pkg-config.
#

find_package (PkgConfig REQUIRED)
pkg_check_modules (SEASTAR REQUIRED seastar)

# Wired up by hand from what pkg-config reported, rather than through
# pkg_check_modules' IMPORTED_TARGET: seastar.pc names its libraries by
# absolute path, which that target carries as link *options*, placing them
# ahead of the object files, where a static archive contributes nothing.
#
# The STATIC_ variables are the ones that include seastar.pc's Libs.private
# and Requires.private, which a static libseastar needs and a shared one
# merely does not benefit from -- and pkg-config cannot say which this
# install is.
add_executable (consumer_pkgconfig consumer.cc)
target_include_directories (consumer_pkgconfig
  PRIVATE
    ${SEASTAR_STATIC_INCLUDE_DIRS})
target_compile_options (consumer_pkgconfig
  PRIVATE
    ${SEASTAR_STATIC_CFLAGS_OTHER})
target_link_libraries (consumer_pkgconfig
  PRIVATE
    ${SEASTAR_STATIC_LDFLAGS})
