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

#
# Copyright (C) 2019 Scylladb, Ltd.
#

include(CMakeParseArguments)

#
# CMake bundles `FindBoost.cmake`, which is coupled to the Boost version. If
# we're on a system without a recent enough version of `FindBoost.cmake`, then
# we need to use the one bundled with Seastar.
#
# The "real" FIND_PACKAGE invocation for Boost is inside SEASTAR_FIND_DEPENDENCIES.
#

# Be consistent in results from FindBoost.cmake.
# This is required because cmake-boost may return to Boost_{component}_LIBRARY:
# - /usr/lib64/libboost_foo.so
# - Boost::foo
set (Boost_NO_BOOST_CMAKE ON)

if (CMAKE_CXX_STANDARD LESS 20)
  set (_seastar_boost_version 1.64.0)
else ()
  # for including the fix of https://github.com/boostorg/test/pull/252
  set (_seastar_boost_version 1.73.0)
endif ()

# This is the minimum version of Boost we need the CMake-bundled `FindBoost.cmake` to know about.
find_package (Boost ${_seastar_boost_version} MODULE)

# - set _seastar_dep_args_<package> for additional args for find_package().
#   add REQUIRED if the corresponding option is explicitly enabled, so
#   find_package() can stop the cmake generation.
# - set _seastar_dep_skip_<package> if the option is explicitly disabled
macro (seastar_set_dep_args package)
  cmake_parse_arguments(args "REQUIRED" "VERSION;OPTION" "COMPONENTS" ${ARGN})
  if (DEFINED args_VERSION)
    list (APPEND _seastar_dep_args_${package} ${args_VERSION})
  endif ()
  if (args_REQUIRED)
    list (APPEND _seastar_dep_args_${package} REQUIRED)
  elseif (DEFINED args_OPTION)
    if (args_OPTION)
      list (APPEND _seastar_dep_args_${package} REQUIRED)
    else ()
      set (_seastar_dep_skip_${package} TRUE)
    endif ()
  endif ()
  if (args_COMPONENTS)
    list (APPEND _seastar_dep_args_${package} COMPONENTS
      ${args_COMPONENTS})
  endif ()
endmacro ()

#
# Iterate through the dependency list defined below and execute `find_package`
# with the corresponding configuration for each 3rd-party dependency.
#
macro (seastar_find_dependencies)
  #
  # List of Seastar dependencies that is meant to be used
  # both in Seastar configuration and by clients which
  # consume Seastar via SeastarConfig.cmake.
  #
  set (_seastar_all_dependencies
    # Public dependencies.
    Boost
    c-ares
    cryptopp
    dpdk # No version information published.
    fmt
    lz4
    # Private and private/public dependencies.
    Concepts
    GnuTLS
    LibUring
    LinuxMembarrier
    Sanitizers
    SourceLocation
    StdAtomic
    hwloc
    lksctp-tools # No version information published.
    numactl # No version information published.
    rt
    yaml-cpp)

  # Arguments to `find_package` for each 3rd-party dependency.
  # Note that the version specification is a "minimal" version requirement.

  # `unit_test_framework` is not required in the case we are building Seastar
  # without the testing library, however the component is always specified as required
  # to keep the CMake code minimalistic and easy-to-use.
  seastar_set_dep_args (Boost REQUIRED
    VERSION ${_seastar_boost_version}
    COMPONENTS
      filesystem
      program_options
      thread
      unit_test_framework)
  seastar_set_dep_args (c-ares REQUIRED
    VERSION 1.13)
  seastar_set_dep_args (cryptopp REQUIRED
    VERSION 5.6.5)
  seastar_set_dep_args (dpdk
    OPTION ${Seastar_DPDK})
  seastar_set_dep_args (fmt REQUIRED
    VERSION 5.0.0)
  seastar_set_dep_args (lz4 REQUIRED
    VERSION 1.7.3)
  seastar_set_dep_args (GnuTLS REQUIRED
    VERSION 3.3.26)
  seastar_set_dep_args (LibUring
    VERSION 2.0
    OPTION ${Seastar_IO_URING})
  seastar_set_dep_args (StdAtomic REQUIRED)
  seastar_set_dep_args (hwloc
    VERSION 1.11.2
    OPTION ${Seastar_HWLOC})
  seastar_set_dep_args (lksctp-tools REQUIRED)
  seastar_set_dep_args (rt REQUIRED)
  seastar_set_dep_args (numactl
    OPTION ${Seastar_NUMA})
  seastar_set_dep_args (yaml-cpp REQUIRED
    VERSION 0.5.1)

  foreach (third_party ${_seastar_all_dependencies})
    if (NOT _seastar_dep_skip_${third_party})
      find_package ("${third_party}" ${_seastar_dep_args_${third_party}})
    endif ()
  endforeach ()
endmacro ()
