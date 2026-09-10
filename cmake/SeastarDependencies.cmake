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

# This is required because cmake-boost may return to Boost_{component}_LIBRARY:
# - /usr/lib64/libboost_foo.so
# - Boost::foo
# While pkgconf's .pc file consumers expect argument which can be passed as
# part of the command line arguments
set (Boost_NO_BOOST_CMAKE ON)

# for including the fix of https://github.com/boostorg/test/pull/252
set (_seastar_boost_version 1.79.0)

# This is the minimum version of Boost we need the CMake-bundled `FindBoost.cmake` to know about.
find_package (Boost ${_seastar_boost_version})
if (Boost_VERSION_STRING VERSION_LESS 1.81.0)
  set_target_properties (Boost::boost PROPERTIES
    INTERFACE_COMPILE_DEFINITIONS "BOOST_NO_CXX98_FUNCTION_BASE")
endif ()

if (CMAKE_FIND_PACKAGE_NAME)
  # used inside find_package(Seastar)
  include (CMakeFindDependencyMacro)

  macro (seastar_find_dep package)
    cmake_parse_arguments(args "REQUIRED" "" "" ${ARGN})
    if (arg_REQUIRED)
      find_dependency (${package} ${arg_UNPARSED_ARGUMENTS})
    else ()
      # some packages are not REQUIRED, so we just check for them instead of
      # populating "REQUIRED" from the original find_package() call.
      find_package (${package} ${ARGN})
    endif ()
  endmacro ()
else()
  macro (seastar_find_dep package)
    # used when configuring Seastar
    find_package (${package} ${ARGN})
  endmacro ()
endif ()

macro (seastar_find_dependencies)
  #
  # List of Seastar dependencies that is meant to be used
  # both in Seastar configuration and by clients which
  # consume Seastar via SeastarConfig.cmake.
  #
  # `unit_test_framework` is not required in the case we are building Seastar
  # without the testing library, however the component is always specified as required
  # to keep the CMake code minimalistic and easy-to-use.
  seastar_find_dep (Boost ${_seastar_boost_version} REQUIRED
    COMPONENTS
      filesystem
      program_options
      thread
      unit_test_framework)
  seastar_find_dep (c-ares 1.13 REQUIRED)
  if (c-ares_VERSION VERSION_GREATER_EQUAL 1.33.0 AND c-ares_VERSION VERSION_LESS 1.34.1)
    # https://github.com/scylladb/seastar/issues/2472
    message (FATAL_ERROR
      "c-ares ${c-ares_VERSION} is not supported. "
      "Seastar requires c-ares version <1.33 or >=1.34.1 ")
  endif ()

  if (Seastar_DPDK)
    seastar_find_dep (dpdk)
  endif()
  seastar_find_dep (fmt 8.1.1 REQUIRED)
  # clang >= 20 enforces consteval strictly enough that fmt < 11 fails to
  # compile fmt/chrono.h's FMT_STRING("{:.{}f}") path. Catch this here
  # rather than letting users hit the cryptic chrono.h error during build.
  if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang"
      AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 20
      AND fmt_VERSION VERSION_LESS 11)
    message (FATAL_ERROR
      "clang ${CMAKE_CXX_COMPILER_VERSION} requires fmt >= 11 "
      "(found ${fmt_VERSION}). Re-run configure.py with --cook fmt "
      "to build a newer fmt locally.")
  endif ()
  if (Seastar_IMPORT_FMT)
    # `import fmt;` needs the module itself, which is a target of its own --
    # separate from the library Seastar links for its own textual includes --
    # and which fmt only defines when built with FMT_MODULE (see
    # cooking_recipe.cmake).  Say so here rather than let the build trip over
    # an `import fmt;` with nothing to import.
    if (NOT TARGET fmt::fmt-module)
      message (FATAL_ERROR
        "Seastar_IMPORT_FMT requires an fmt built with FMT_MODULE, which is what "
        "provides the fmt::fmt-module target. The fmt found in ${fmt_DIR} does not "
        "have it. Re-run configure.py with --cook fmt to build a suitable fmt "
        "locally.")
    endif ()
    # fmt ships the module as source: whoever consumes it compiles the interface
    # unit themselves, so both of the following have to be arranged here --
    # where consumers of the installed package come through too -- and not in
    # Seastar's own CMakeLists.txt.
    #
    # FMT_ATTACH_TO_GLOBAL_MODULE detaches fmt's declarations from module `fmt`,
    # leaving them traditionally mangled and identical to what a textual
    # <fmt/...> include produces. Seastar needs that: its own sources include
    # fmt textually either way, and its ABI is not fmt-free -- logger's
    # failed_to_log() takes an fmt::string_view, and the formatters for
    # log_level, backtraces and exception_ptr are defined out of line in the
    # library -- so an importing translation unit would otherwise reference
    # module-attached names that libseastar does not define. fmt documents the
    # macro for exactly this, mixing importing and including TUs in one program.
    #
    # And fmt exports the module as demanding no more than C++20, which CMake
    # takes literally and compiles the interface unit with -std=c++20; any
    # translation unit on a later standard then refuses the resulting BMI. Ask
    # for the standard actually in use.
    set_property (TARGET fmt::fmt-module APPEND PROPERTY
      IMPORTED_CXX_MODULES_COMPILE_DEFINITIONS FMT_ATTACH_TO_GLOBAL_MODULE)
    set_property (TARGET fmt::fmt-module APPEND PROPERTY
      IMPORTED_CXX_MODULES_COMPILE_FEATURES cxx_std_${CMAKE_CXX_STANDARD})
  endif ()
  seastar_find_dep (lz4 1.7.3 REQUIRED)
  seastar_find_dep (GnuTLS 3.7.4)
  seastar_find_dep (OpenSSL 3.0)
  if (Seastar_IO_URING)
    seastar_find_dep (LibUring 2.0 REQUIRED)
  endif()
  seastar_find_dep (LinuxMembarrier)
  seastar_find_dep (Sanitizers)
  seastar_find_dep (StdAtomic REQUIRED)
  if (Seastar_LTTNG)
    seastar_find_dep (LTTngUST REQUIRED)
  endif ()
  if (Seastar_HWLOC)
    seastar_find_dep (hwloc 1.11.2 REQUIRED)
  endif()
  seastar_find_dep (lksctp-tools REQUIRED)
  seastar_find_dep (rt REQUIRED)
  seastar_find_dep (ucontext REQUIRED)
  seastar_find_dep (yaml-cpp REQUIRED
    VERSION 0.5.1)

  # workaround for https://gitlab.kitware.com/cmake/cmake/-/issues/25079
  # since protobuf v22.0, it started using abseil, see
  # https://github.com/protocolbuffers/protobuf/releases/tag/v22.0 .
  # but due to https://gitlab.kitware.com/cmake/cmake/-/issues/25079,
  # CMake's FindProtobuf does add this linkage yet. fortunately,
  # ProtobufConfig.cmake provided by protobuf defines this linkage. so we try
  # the CMake package configuration file first, and fall back to CMake's
  # FindProtobuf module.
  find_package (Protobuf QUIET CONFIG)
  if (Protobuf_FOUND AND Protobuf_VERSION VERSION_GREATER_EQUAL 2.5.0)
    # do it again, so the message is printed when the package is found
    seastar_find_dep (Protobuf CONFIG REQUIRED)
  else ()
    seastar_find_dep (Protobuf 2.5.0 REQUIRED)
  endif ()

endmacro ()
