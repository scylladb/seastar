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

add_library (seastar-testing
  test-utils.cc
  test_runner.cc)

target_include_directories (seastar-testing PUBLIC
  $<TARGET_PROPERTY:seastar,INCLUDE_DIRECTORIES>)

if (NOT ${SEASTAR_LINK_STATIC_BOOST})
  target_compile_definitions (seastar-testing PUBLIC
    BOOST_TEST_DYN_LINK)
endif ()

# Logical target for all tests.
add_custom_target (tests)
# Logical target for all tests with debugging symbols.
add_custom_target (tests_debug)
set_target_properties (tests_debug PROPERTIES EXCLUDE_FROM_ALL ON)

# ADD_SEASTAR_TEST NAME $name [CUSTOM] [SUITE] SOURCES $source_1 $source_2 ... $source_n ARGS $arg_1 $arg_2 ... $arg_n)
#
# Define a unit test with name `name`. This defines a build target with `name`.
#
# If CUSTOM is included, then the test doesn't link against the Seastar testing libraries.
#
# if SUITE is included, the test is automatically run as part of the test suite (and in Jenkins).
#
# SOURCES are the source files necessary for the test.
#
# ARGS are additional arguments that the executable should be invoked with when the test is run.
function (add_seastar_test)
  set (options CUSTOM SUITE)
  set (one_value_args NAME)
  set (multi_value_args SOURCES ARGS)
  cmake_parse_arguments (args "${options}" "${one_value_args}" "${multi_value_args}" "${ARGN}")

  macro (prepare name)
    add_executable (${name} "${args_SOURCES}")

    target_link_libraries (${name} PUBLIC
      -Wl,--whole-archive seastar -Wl,--no-whole-archive
      Boost::unit_test_framework)

    if (NOT "${args_CUSTOM}")
      target_link_libraries (${name} PUBLIC
        -Wl,--whole-archive seastar-testing -Wl,--no-whole-archive)
    endif ()
  endmacro ()

  prepare (${args_NAME})
  add_dependencies (tests ${args_NAME})
  # Strip symbols.
  target_link_libraries (${args_NAME} PUBLIC -s)

  if ("${args_SUITE}")
    if ((NOT ("${SEASTAR_JENKINS}" STREQUAL "")) AND (NOT "${args_CUSTOM}"))
      string (TOLOWER ${CMAKE_BUILD_TYPE} jenkins_mode)
      set (jenkins_file_name "${SEASTAR_JENKINS}.${jenkins_mode}.${args_NAME}.boost.xml")
      set (jenkins_args --output_format=XML --log_level=all --report_level=no --log_sink=${jenkins_file_name})
      set (args_ARGS -- "${args_ARGS}")
    endif ()

    add_test (
      NAME ${args_NAME}
      COMMAND ${args_NAME} ${jenkins_args} ${args_ARGS}
      WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/..")
  endif ()

  set (name_with_symbols "${args_NAME}_g")
  prepare (${name_with_symbols})
  add_dependencies (tests_debug ${name_with_symbols})
  set_target_properties (${name_with_symbols} PROPERTIES EXCLUDE_FROM_ALL ON)
endfunction ()

#
# Certificates for TLS tests.
#

configure_file (catest.key . COPYONLY)
configure_file (catest.pem . COPYONLY)
configure_file (tls-ca-bundle.pem . COPYONLY)
configure_file (test.crl . COPYONLY)
configure_file (test.crt . COPYONLY)
configure_file (test.csr . COPYONLY)
configure_file (test.key . COPYONLY)

add_seastar_test (NAME abort_source_test
  SUITE
  SOURCES abort_source_test.cc)

add_seastar_test (NAME alloc_test
  SUITE
  SOURCES alloc_test.cc)

add_seastar_test (NAME alien_test
  SUITE
  CUSTOM
  SOURCES alien_test.cc)

if (${SEASTAR_EXECUTE_ONLY_FAST_TESTS})
  if (${CMAKE_BUILD_TYPE} STREQUAL Debug)
    set (allocator_test_args --iterations 5)
  else ()
    set (allocator_test_args --time 0.1)
  endif ()
endif ()
add_seastar_test (NAME allocator_test
  ARGS "${allocator_test_args}"
  SUITE
  CUSTOM
  SOURCES allocator_test.cc)

add_seastar_test (NAME blkdiscard_test
  CUSTOM
  SOURCES blkdiscard_test.cc)

add_seastar_test (NAME checked_ptr_test
  CUSTOM
  SOURCES checked_ptr_test.cc)

add_seastar_test (NAME chunked_fifo_test
  CUSTOM SOURCES
  chunked_fifo_test.cc)

add_seastar_test (NAME circular_buffer_test
  CUSTOM
  SOURCES circular_buffer_test.cc)

add_seastar_test (NAME connect_test
  SUITE
  SOURCES connect_test.cc)

add_seastar_test (NAME defer_test
  SUITE
  CUSTOM
  SOURCES defer_test.cc)

add_seastar_test (NAME directory_test
  SUITE
  CUSTOM
  SOURCES directory_test.cc)

add_seastar_test (NAME distributed_test
  ARGS -c 2
  SUITE
  CUSTOM
  SOURCES distributed_test.cc)

add_seastar_test (NAME dns_test
  SOURCES dns_test.cc)

add_seastar_test (NAME echo_test
  CUSTOM
  SOURCES echotest.cc)

add_seastar_test (NAME execution_stage_test
  SUITE
  SOURCES execution_stage_test.cc)

add_seastar_test (NAME expiring_fifo_test
  SUITE
  SOURCES expiring_fifo_test.cc)

add_seastar_test (NAME fair_queue_test
  SUITE
  SOURCES fair_queue_test.cc)

add_seastar_test (NAME file_io_test
  SOURCES fileiotest.cc)

add_seastar_test (NAME foreign_ptr_test
  SUITE SOURCES foreign_ptr_test.cc)

add_seastar_test (NAME fstream_test
  SUITE
  SOURCES fstream_test.cc)

add_seastar_test (NAME futures_test
  SUITE
  SOURCES futures_test.cc)

add_seastar_test (NAME httpd_test
  SUITE
  SOURCES httpd.cc)

add_seastar_test (NAME ip_test
  CUSTOM
  SOURCES ip_test.cc)

add_seastar_test (NAME json_formatter_test
  SUITE
  SOURCES json_formatter_test.cc)

add_seastar_test (NAME l3_test
  CUSTOM
  SOURCES l3_test.cc)

add_seastar_test (NAME lowres_clock_test
  SUITE
  SOURCES lowres_clock_test.cc)

add_seastar_test (NAME noncopyable_function_test
  SUITE
  CUSTOM
  SOURCES noncopyable_function_test.cc)

add_seastar_test (NAME output_stream_test
  SOURCES output_stream_test.cc)

add_seastar_test (NAME packet_test
  SUITE
  CUSTOM SOURCES packet_test.cc)

add_seastar_test (NAME program_options_test
  SUITE
  CUSTOM
  SOURCES program_options_test.cc)

add_seastar_test (NAME rpc_test
  SUITE
  SOURCES rpc_test.cc)

add_seastar_test (NAME scheduling_group_demo
  CUSTOM
  SOURCES scheduling_group_demo.cc)

add_seastar_test (NAME semaphore_test
  SUITE
  SOURCES semaphore_test.cc)

add_seastar_test (NAME shared_ptr_test
  SUITE
  CUSTOM
  SOURCES shared_ptr_test.cc)

add_seastar_test (NAME slab_test
  CUSTOM
  SOURCES slab_test.cc)

add_seastar_test (NAME smp_test
  SUITE
  CUSTOM
  SOURCES smp_test.cc)

add_seastar_test (NAME sstring_test
  SUITE
  CUSTOM
  SOURCES sstring_test.cc)

add_seastar_test (NAME tcp_test
  CUSTOM
  SOURCES tcp_test.cc)

add_seastar_test (NAME thread_test
  SUITE
  SOURCES thread_test.cc)

add_seastar_test (NAME thread_context_switch_test
  SUITE
  CUSTOM
  SOURCES thread_context_switch.cc)

add_seastar_test (NAME timer_test
  SUITE
  CUSTOM
  SOURCES timertest.cc)

add_seastar_test (NAME tls_test SUITE
  SOURCES tls_test.cc)

add_seastar_test (NAME tuple_utils_test
  SUITE
  CUSTOM
  SOURCES tuple_utils_test)

add_seastar_test (NAME unwind_test
  CUSTOM
  SOURCES unwind_test.cc)

add_seastar_test (NAME weak_ptr_test
  SUITE
  CUSTOM
  SOURCES weak_ptr_test.cc)

# Disable false positive due to new (with_alignment(...)).
set (CTEST_ENVIRONMENT
  ASAN_OPTIONS='alloc_dealloc_mismatch=0')

add_subdirectory (perf)
add_subdirectory (memcached)
