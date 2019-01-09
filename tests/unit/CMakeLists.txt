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

# Logical target for all unit tests.
add_custom_target (unit_tests)

macro (seastar_add_test name)
  set (args ${ARGN})

  cmake_parse_arguments (parsed_args
    "NO_SEASTAR_TESTING_LIBRARY"
    "WORKING_DIRECTORY"
    "RUN_ARGS;SOURCES"
    ${args})

  set (command_args "")
  set (depends_args "")

  if (parsed_args_SOURCES)
    if (parsed_args_NO_SEASTAR_TESTING_LIBRARY)
      set (libraries seastar_with_flags)

      if (parsed_args_RUN_ARGS)
        set (run_args ${parsed_args_RUN_ARGS})
      else ()
        set (run_args -c 2)
      endif ()
    else ()
      set (libraries
        seastar_with_flags
        seastar_testing)

      if (NOT (Seastar_JENKINS STREQUAL ""))
        seastar_jenkins_arguments (${name} jenkins_args)
      else ()
        set (jenkins_args "")
      endif ()

      if (parsed_args_RUN_ARGS)
        set (test_args ${parsed_args_RUN_ARGS})
      else ()
        set (test_args -- -c 2)
      endif ()

      set (run_args
        ${jenkins_args}
        ${test_args})
    endif ()

    set (executable_target test_unit_${name})
    add_executable (${executable_target} ${parsed_args_SOURCES})

    target_link_libraries (${executable_target}
      PRIVATE ${libraries})

    target_compile_definitions (${executable_target}
      PRIVATE SEASTAR_TESTING_MAIN)

    target_include_directories (${executable_target}
      PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${Seastar_SOURCE_DIR}/src)

    set_target_properties (${executable_target}
      PROPERTIES
        OUTPUT_NAME ${name})

    add_dependencies (unit_tests ${executable_target})
    list (APPEND command_args COMMAND ${executable_target} ${run_args})
  endif ()

  set (target test_unit_${name}_run)

  if (parsed_args_WORKING_DIRECTORY)
    list (APPEND command_args WORKING_DIRECTORY ${parsed_args_WORKING_DIRECTORY})
  endif ()

  add_custom_target (${target}
    ${command_args}
    ${parsed_args_UNPARSED_ARGUMENTS}
    USES_TERMINAL)

  add_test (
    NAME Seastar.unit.${name}
    COMMAND ${CMAKE_COMMAND} --build ${Seastar_BINARY_DIR} --target ${target})

  set_tests_properties (Seastar.unit.${name}
    PROPERTIES
      TIMEOUT ${Seastar_TEST_TIMEOUT})
endmacro ()

function (prepend_each var prefix)
  set (result "")

  foreach (x ${ARGN})
    list (APPEND result ${prefix}/${x})
  endforeach ()

  set (${var} ${result} PARENT_SCOPE)
endfunction ()

add_custom_target (test_unit
  COMMAND ctest --verbose -R Seastar.unit
  USES_TERMINAL)

seastar_add_test (abort_source
  SOURCES abort_source_test.cc)

seastar_add_test (alloc
  SOURCES alloc_test.cc)

if (NOT Seastar_EXECUTE_ONLY_FAST_TESTS)
  set (allocator_test_args "")
else ()
  if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    set (allocator_test_args --iterations 5)
  else ()
    set (allocator_test_args --time 0.1)
  endif ()
endif ()

seastar_add_test (allocator
  SOURCES allocator_test.cc
  RUN_ARGS ${allocator_test_args})

seastar_add_test (alien
  SOURCES alien_test.cc
  NO_SEASTAR_TESTING_LIBRARY)

seastar_add_test (checked_ptr
  SOURCES checked_ptr_test.cc)

seastar_add_test (chunked_fifo
  SOURCES chunked_fifo_test.cc)

seastar_add_test (circular_buffer
  SOURCES circular_buffer_test.cc)

seastar_add_test (circular_buffer_fixed_capacity
  SOURCES circular_buffer_fixed_capacity_test.cc)

seastar_add_test (connect
  SOURCES connect_test.cc)

seastar_add_test (defer
  SOURCES defer_test.cc)

seastar_add_test (directory
  SOURCES directory_test.cc
  NO_SEASTAR_TESTING_LIBRARY)

seastar_add_test (distributed
  SOURCES distributed_test.cc
  NO_SEASTAR_TESTING_LIBRARY)

seastar_add_test (dns
  SOURCES dns_test.cc)

seastar_add_test (execution_stage
  SOURCES execution_stage_test.cc)

seastar_add_test (expiring_fifo
  SOURCES expiring_fifo_test.cc)

seastar_add_test (fair_queue
  SOURCES fair_queue_test.cc)

seastar_add_test (file_io
  SOURCES file_io_test.cc)

seastar_add_test (foreign_ptr
  SOURCES foreign_ptr_test.cc)

seastar_add_test (fstream
  SOURCES
    fstream_test.cc
    mock_file.hh)

seastar_add_test (futures
  SOURCES futures_test.cc)

seastar_add_test (httpd
  SOURCES
    httpd_test.cc
    loopback_socket.hh)

seastar_add_test (json_formatter
  SOURCES json_formatter_test.cc)

seastar_add_test (lowres_clock
  SOURCES lowres_clock_test.cc)

seastar_add_test (net_config
  SOURCES net_config_test.cc)

seastar_add_test (noncopyable_function
  SOURCES noncopyable_function_test.cc)

seastar_add_test (output_stream
  SOURCES output_stream_test.cc)

seastar_add_test (packet
  SOURCES packet_test.cc)

seastar_add_test (program_options
  SOURCES program_options_test.cc)

seastar_add_test (queue
  SOURCES queue_test.cc)

seastar_add_test (rpc
  SOURCES
    loopback_socket.hh
    rpc_test.cc)

seastar_add_test (semaphore
  SOURCES semaphore_test.cc)

seastar_add_test (shared_ptr
  SOURCES shared_ptr_test.cc)

seastar_add_test (signal
  SOURCES signal_test.cc)

seastar_add_test (simple_stream
  SOURCES simple_stream_test.cc)

# TODO: Disabled for now. See GH-520.
# seastar_add_test (slab
#   SOURCES slab_test.cc
#   NO_SEASTAR_TESTING_LIBRARY)

seastar_add_test (smp
  SOURCES smp_test.cc
  NO_SEASTAR_TESTING_LIBRARY)

seastar_add_test (sstring
  SOURCES sstring_test.cc)

seastar_add_test (stall_detector
  SOURCES stall_detector_test.cc)

seastar_add_test (thread
  SOURCES thread_test.cc)

seastar_add_test (thread_context_switch
  SOURCES thread_context_switch_test.cc
  NO_SEASTAR_TESTING_LIBRARY)

seastar_add_test (timer
  SOURCES timer_test.cc
  NO_SEASTAR_TESTING_LIBRARY)

set (tls_certificate_files
  catest.key
  catest.pem
  tls-ca-bundle.pem
  test.crl
  test.crt
  test.csr
  test.key)

prepend_each (
  in_tls_certificate_files
  ${CMAKE_CURRENT_SOURCE_DIR}/
  ${tls_certificate_files})

prepend_each (
  out_tls_certificate_files
  ${CMAKE_CURRENT_BINARY_DIR}/
  ${tls_certificate_files})

add_custom_command (
  DEPENDS ${in_tls_certificate_files}
  OUTPUT ${out_tls_certificate_files}
  COMMAND ${CMAKE_COMMAND} -E copy ${in_tls_certificate_files} ${CMAKE_CURRENT_BINARY_DIR})

# TODO: Disabled for now. See GH-514.
# seastar_add_test (tls
#   DEPENDS ${out_tls_certificate_files}
#   SOURCES tls_test.cc
#   WORKING_DIRECTORY ${Seastar_BINARY_DIR})

seastar_add_test (tuple_utils
  SOURCES tuple_utils_test.cc)

seastar_add_test (unwind
  SOURCES unwind_test.cc)

seastar_add_test (weak_ptr
  SOURCES weak_ptr_test.cc)
