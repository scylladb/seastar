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
    set (libraries seastar_private)
  else ()
    set (libraries
      seastar_private
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
      OUTPUT_NAME ${name}_perf)

  add_dependencies (perf_tests ${target})
  set (${name}_test ${target})
endmacro ()

seastar_add_test (fstream
  SOURCES fstream_perf.cc
  NO_SEASTAR_PERF_TESTING_LIBRARY)

seastar_add_test (fair_queue
  SOURCES fair_queue_perf.cc)

seastar_add_test (shared_token_bucket
  SOURCES shared_token_bucket.cc)

seastar_add_test (future_util
  SOURCES future_util_perf.cc)

seastar_add_test (rpc
  SOURCES rpc_perf.cc)

seastar_add_test (smp_submit_to
  SOURCES smp_submit_to_perf.cc
  NO_SEASTAR_PERF_TESTING_LIBRARY)

seastar_add_test (coroutine
  SOURCES coroutine_perf.cc)

seastar_add_test (allocator
  SOURCES allocator_perf.cc)

seastar_add_test (container
  SOURCES container_perf.cc)

seastar_add_test (http_client
  SOURCES http_client_perf.cc linux_perf_event.cc
  NO_SEASTAR_PERF_TESTING_LIBRARY)

seastar_add_test (perf_tests
  SOURCES perf_tests_perf.cc)
