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
# Copyright (C) 2018 Scylladb, Ltd.
#

add_library (seastar_perf_testing
  perf_tests.hh
  perf_tests.cc)

target_link_libraries (seastar_perf_testing
  PUBLIC
    fmt::fmt
    seastar_with_flags)

# Logical target for all perf tests.
add_custom_target (perf_tests)

macro (seastar_add_test name)
  set (args ${ARGN})

  cmake_parse_arguments (
    parsed_args
    "NO_SEASTAR_PERF_TESTING_LIBRARY"
    ""
    "SOURCES"
    ${args})

  set (target test_perf_${name})
  add_executable (${target} ${parsed_args_SOURCES})

  if (parsed_args_NO_SEASTAR_PERF_TESTING_LIBRARY)
    set (libraries seastar_with_flags)
  else ()
    set (libraries
      seastar_with_flags
      seastar_perf_testing)
  endif ()

  target_link_libraries (${target}
    PRIVATE ${libraries})

  target_include_directories (${target}
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}
      ${Seastar_SOURCE_DIR}/src)

  set_target_properties (${target}
    PROPERTIES
      OUTPUT_NAME ${name})

  add_dependencies (perf_tests ${target})
  set (${name}_test ${target})
endmacro ()

seastar_add_test (fstream
  SOURCES fstream_perf.cc
  NO_SEASTAR_PERF_TESTING_LIBRARY)

seastar_add_test (future_util
  SOURCES future_util_perf.cc)
