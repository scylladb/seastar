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
# (find_package (Seastar), Seastar::seastar) and pkg-config (seastar.pc). Both
# carry compile definitions and flags that Seastar's own build tree does not
# apply to its own targets, Seastar_IMPORT_FMT's among them, so nothing in the
# main build exercises them.
#
# It is not part of Seastar's build. Install Seastar somewhere, then:
#
#   cmake -G Ninja -S tests/consumer -B /tmp/consumer \
#         -DCMAKE_PREFIX_PATH=<install prefix> -DCMAKE_CXX_STANDARD=<same as Seastar>
#   cmake --build /tmp/consumer
#
# Building is the test: a consumer that cannot find the headers, is handed the
# wrong flags, or ends up with fmt names Seastar's library does not define
# fails to compile or link here.
#
# Doubles as the worked example of what an application has to do for
# Seastar_IMPORT_FMT, which is more than linking Seastar -- see the fmt module
# handling below.

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
# Seastar builds with GNU extensions on, and a translation unit that disagrees
# about them refuses a module BMI. seastar.pc names a standard of its own,
# which lands after this one and wins for that channel -- harmlessly, since it
# then applies to every source in the target, module interface unit included,
# which is what the BMI needs.
set (CMAKE_CXX_EXTENSIONS ON)

# `import fmt;` (Seastar_IMPORT_FMT) needs the module's interface unit compiled
# into a BMI, and that is on the consumer: CMake resolves an import only
# against the module-providing targets a target links *directly*, so Seastar
# cannot pass fmt's along on its interface. Each channel below therefore has to
# arrange it, and neither should when Seastar was built the ordinary way -- the
# textual <fmt/*.h> that Seastar's headers then include would collide with an
# import of the same library.
#
# Whether it applies is read off the definitions the channel hands over, which
# is the same thing <seastar/core/internal/fmt.hh> keys off.
function (consumer_uses_fmt_module out definitions)
  if ("SEASTAR_IMPORT_FMT" IN_LIST definitions OR "-DSEASTAR_IMPORT_FMT" IN_LIST definitions)
    set (${out} TRUE PARENT_SCOPE)
  else ()
    set (${out} FALSE PARENT_SCOPE)
  endif ()
endfunction ()

#
# Channel 1: the CMake package.
#

find_package (Seastar REQUIRED)

add_executable (consumer_cmake consumer.cc)
target_link_libraries (consumer_cmake
  PRIVATE
    Seastar::seastar)

get_target_property (seastar_definitions Seastar::seastar INTERFACE_COMPILE_DEFINITIONS)
consumer_uses_fmt_module (cmake_needs_fmt_module "${seastar_definitions}")
if (cmake_needs_fmt_module)
  # find_package (Seastar) has already found fmt and given fmt::fmt-module the
  # properties an importing build needs, so linking it is all that is left.
  if (NOT TARGET fmt::fmt-module)
    message (FATAL_ERROR
      "Seastar was installed with Seastar_IMPORT_FMT but find_package (Seastar) "
      "did not define fmt::fmt-module.")
  endif ()
  target_link_libraries (consumer_cmake
    PRIVATE
      fmt::fmt-module)
  set_target_properties (consumer_cmake
    PROPERTIES
      CXX_SCAN_FOR_MODULES ON)
endif ()

#
# Channel 2: pkg-config.
#

find_package (PkgConfig REQUIRED)
pkg_check_modules (SEASTAR REQUIRED seastar)

# Wired up by hand from what pkg-config reported, rather than through
# pkg_check_modules' IMPORTED_TARGET: seastar.pc names its libraries by
# absolute path, which that target carries as link *options*, placing them
# ahead of the object files. A static archive there contributes nothing, and
# the fmt Seastar links is one.
add_executable (consumer_pkgconfig consumer.cc)
target_include_directories (consumer_pkgconfig
  PRIVATE
    ${SEASTAR_INCLUDE_DIRS})
target_compile_options (consumer_pkgconfig
  PRIVATE
    ${SEASTAR_CFLAGS_OTHER})
target_link_libraries (consumer_pkgconfig
  PRIVATE
    ${SEASTAR_LDFLAGS})

consumer_uses_fmt_module (pkgconfig_needs_fmt_module "${SEASTAR_CFLAGS_OTHER}")
if (pkgconfig_needs_fmt_module)
  # No targets come over this channel, so the module interface unit is compiled
  # as part of this executable, from the source fmt installs alongside its
  # headers. The flags it needs -- FMT_ATTACH_TO_GLOBAL_MODULE in particular --
  # are in seastar.pc's Cflags and so already apply to it.
  find_file (FMT_MODULE_SOURCE fmt/src/fmt.cc
    PATHS ${SEASTAR_INCLUDE_DIRS}
    NO_DEFAULT_PATH)
  if (NOT FMT_MODULE_SOURCE)
    message (FATAL_ERROR
      "Seastar was installed with Seastar_IMPORT_FMT but fmt's module source "
      "(fmt/src/fmt.cc) is not among the include directories seastar.pc names: "
      "${SEASTAR_INCLUDE_DIRS}. It is installed by an fmt built with FMT_MODULE.")
  endif ()
  cmake_path (GET FMT_MODULE_SOURCE PARENT_PATH fmt_module_dir)
  target_sources (consumer_pkgconfig
    PRIVATE
      FILE_SET fmt_module
      TYPE CXX_MODULES
      BASE_DIRS ${fmt_module_dir}
      FILES ${FMT_MODULE_SOURCE})
endif ()
