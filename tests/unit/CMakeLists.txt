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

set (Seastar_UNIT_TEST_SMP
  2
  CACHE
  STRING
  "Run unit tests with this many cores.")

#
# Define a new unit test with the given name.
#
# seastar_add_test (name
#   [KIND {SEASTAR,BOOST,CUSTOM}]
#   [SOURCES source1 source2 ... sourcen]
#   [WORKING_DIRECTORY dir]
#   [LIBRARIES library1 library2 ... libraryn]
#   [RUN_ARGS arg1 arg2 ... argn])
#
# There are three kinds of test we support (the KIND parameter):
#
# - SEASTAR: Unit tests which use macros like `SEASTAR_TEST_CASE`
# - BOOST: Unit tests which use macros like `BOOST_AUTO_TEST_CASE`
# - CUSTOM: Custom tests which need to be specified
#
# SEASTAR and BOOST tests will have their output saved for interpretation by the Jenkins continuous integration service
# if this is configured for the build.
#
# KIND can be omitted, in which case it is assumed to be SEASTAR.
#
# If SOURCES is provided, then the test files are first compiled into an executable which has the same name as the test
# but with a suffix ("_test").
#
# WORKING_DIRECTORY can be optionally provided to choose where the test is executed.
#
# If LIBRARIES is provided along with SOURCES, then the executable is additionally linked with these libraries.
#
# RUN_ARGS are optional additional arguments to pass to the executable. For SEASTAR tests, these come after `--`. For
# CUSTOM tests with no SOURCES, this parameter can be used to specify the executable name as well as its arguments since
# no executable is compiled.
#
function (seastar_add_test name)
  set (test_kinds
    SEASTAR
    BOOST
    CUSTOM)

  cmake_parse_arguments (parsed_args
    ""
    "WORKING_DIRECTORY;KIND"
    "RUN_ARGS;SOURCES;LIBRARIES;DEPENDS"
    ${ARGN})

  if (NOT parsed_args_KIND)
    set (parsed_args_KIND SEASTAR)
  elseif (NOT (parsed_args_KIND IN_LIST test_kinds))
    message (FATAL_ERROR "Invalid test kind. KIND must be one of ${test_kinds}")
  endif ()

  if (parsed_args_SOURCES)
    # These may be unused.
    seastar_jenkins_arguments (${name} jenkins_args)

    #
    # Each kind of test must populate the `args` and `libraries` lists.
    #

    set (libraries "${parsed_args_LIBRARIES}")

    set (args "")
    if (parsed_args_KIND STREQUAL "SEASTAR")
      list (APPEND libraries
        seastar_testing
        seastar_private)

      if (NOT (Seastar_JENKINS STREQUAL ""))
        list (APPEND args ${jenkins_args})
      endif ()

      list (APPEND args -- -c ${Seastar_UNIT_TEST_SMP})
    elseif (parsed_args_KIND STREQUAL "BOOST")
      list (APPEND libraries
        Boost::unit_test_framework
        seastar_private)

      if (NOT (Seastar_JENKINS STREQUAL ""))
        list (APPEND args ${jenkins_args})
      endif ()
    endif ()

    list (APPEND args ${parsed_args_RUN_ARGS})

    set (executable_target test_unit_${name})
    add_executable (${executable_target} ${parsed_args_SOURCES})

    target_link_libraries (${executable_target}
      PRIVATE ${libraries})

    target_compile_definitions (${executable_target}
      PRIVATE SEASTAR_TESTING_MAIN)

    if ((Seastar_STACK_GUARDS STREQUAL "ON") OR
        ((Seastar_STACK_GUARDS STREQUAL "DEFAULT") AND
         (CMAKE_BUILD_TYPE IN_LIST Seastar_STACK_GUARD_MODES)))
      target_compile_definitions (${executable_target}
        PRIVATE
          SEASTAR_THREAD_STACK_GUARDS)
    endif ()

    target_include_directories (${executable_target}
      PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${Seastar_SOURCE_DIR}/src)

    set_target_properties (${executable_target}
      PROPERTIES
        OUTPUT_NAME ${name}_test)

    add_dependencies (unit_tests ${executable_target})
    set (forwarded_args COMMAND ${executable_target} ${args})
  else ()
    if (NOT (parsed_args_KIND STREQUAL "CUSTOM"))
      message (FATAL_ERROR "SOURCES are required for ${parsed_args_KIND} tests")
    endif ()

    set (forwarded_args COMMAND ${parsed_args_RUN_ARGS})
  endif ()

  #
  # We expect `forwarded_args` to be populated correctly at this point.
  #

  set (target test_unit_${name}_run)

  if (parsed_args_WORKING_DIRECTORY)
    list (APPEND forwarded_args WORKING_DIRECTORY ${parsed_args_WORKING_DIRECTORY})
  endif ()

  if (parsed_args_DEPENDS)
    add_dependencies(${executable_target} ${parsed_args_DEPENDS})
  endif()

  add_custom_target (${target}
    ${forwarded_args}
    USES_TERMINAL)

  add_test (
    NAME Seastar.unit.${name}
    COMMAND ${CMAKE_COMMAND} --build ${Seastar_BINARY_DIR} --target ${target})

  set_tests_properties (Seastar.unit.${name}
    PROPERTIES
      TIMEOUT ${Seastar_TEST_TIMEOUT}
      ENVIRONMENT "${Seastar_TEST_ENVIRONMENT}")
endfunction ()

#
# Define a new custom unit test whose entry point is a Seastar application.
#
# seastar_add_app_test (name
#   [SOURCES source1 source2 ... sourcen]
#   [LIBRARIES library1 library2 ... libraryn]
#   [RUN_ARGS arg1 arg2 ... argn])
#
# These kinds of tests are structured like Seastar applications.
#
# These tests always link against `seastar_private` and are always invoked with
# `-c ${Seastar_UNIT_TEST_SMP}`.
#
function (seastar_add_app_test name)
  cmake_parse_arguments (parsed_args
    ""
    ""
    "RUN_ARGS;SOURCES;LIBRARIES"
    ${ARGN})

  seastar_add_test (${name}
    KIND CUSTOM
    SOURCES ${parsed_args_SOURCES}
    LIBRARIES
      seastar_private
      ${parsed_args_LIBRARIES}
    RUN_ARGS
      -c ${Seastar_UNIT_TEST_SMP}
      ${parsed_args_RUN_ARGS})
endfunction ()

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

seastar_add_app_test (alien
  SOURCES alien_test.cc)

seastar_add_test (checked_ptr
  SOURCES checked_ptr_test.cc)

seastar_add_test (chunked_fifo
  SOURCES chunked_fifo_test.cc)

seastar_add_test (chunk_parsers
  SOURCES chunk_parsers_test.cc)

seastar_add_test (circular_buffer
  SOURCES circular_buffer_test.cc)

seastar_add_test (circular_buffer_fixed_capacity
  SOURCES circular_buffer_fixed_capacity_test.cc)

seastar_add_test (connect
  SOURCES connect_test.cc)

seastar_add_test (content_source
  SOURCES content_source_test.cc)

seastar_add_test (coroutines
  SOURCES coroutines_test.cc)

seastar_add_test (defer
  SOURCES defer_test.cc)

seastar_add_test (deleter
  SOURCES deleter_test.cc)

seastar_add_app_test (directory
  SOURCES directory_test.cc)

seastar_add_test (distributed
  SOURCES distributed_test.cc)

seastar_add_test (dns
  SOURCES dns_test.cc)

seastar_add_test (execution_stage
  SOURCES execution_stage_test.cc)

seastar_add_test (expiring_fifo
  SOURCES expiring_fifo_test.cc)

seastar_add_test (io_queue
  SOURCES io_queue_test.cc)

seastar_add_test (fair_queue
  SOURCES fair_queue_test.cc)

seastar_add_test (file_io
  SOURCES file_io_test.cc)

seastar_add_test (file_utils
  SOURCES file_utils_test.cc)

seastar_add_test (foreign_ptr
  SOURCES foreign_ptr_test.cc)

seastar_add_test (fsnotifier
  SOURCES fsnotifier_test.cc)

seastar_add_test (fstream
  SOURCES
    fstream_test.cc
    mock_file.hh)

seastar_add_test (futures
  SOURCES futures_test.cc)

seastar_add_test (sharded
  SOURCES sharded_test.cc)

seastar_add_test (httpd
  SOURCES
    httpd_test.cc
    loopback_socket.hh)

seastar_add_test (websocket
  SOURCES websocket_test.cc)

seastar_add_test (ipv6
  SOURCES ipv6_test.cc)

seastar_add_test (network_interface
  SOURCES network_interface_test.cc)

seastar_add_test (json_formatter
  SOURCES json_formatter_test.cc)

seastar_add_test (locking
  SOURCES locking_test.cc)

seastar_add_test (lowres_clock
  SOURCES lowres_clock_test.cc)

seastar_add_test (metrics
  SOURCES metrics_test.cc)

seastar_add_test (net_config
  KIND BOOST
  SOURCES net_config_test.cc)

seastar_add_test (noncopyable_function
  KIND BOOST
  SOURCES noncopyable_function_test.cc)

seastar_add_test (output_stream
  SOURCES output_stream_test.cc)

seastar_add_test (packet
  KIND BOOST
  SOURCES packet_test.cc)

seastar_add_test (program_options
  KIND BOOST
  SOURCES program_options_test.cc)

seastar_add_test (queue
  SOURCES queue_test.cc)

seastar_add_test (request_parser
  SOURCES request_parser_test.cc)

seastar_add_test (rpc
  SOURCES
    loopback_socket.hh
    rpc_test.cc)

seastar_add_test (semaphore
  SOURCES semaphore_test.cc)

seastar_add_test (shared_ptr
  KIND BOOST
  SOURCES shared_ptr_test.cc)

seastar_add_test (signal
  SOURCES signal_test.cc)

seastar_add_test (simple_stream
  KIND BOOST
  SOURCES simple_stream_test.cc)

# TODO: Disabled for now. See GH-520.
# seastar_add_test (slab
#   SOURCES slab_test.cc
#   NO_SEASTAR_TESTING_LIBRARY)

seastar_add_app_test (smp
  SOURCES smp_test.cc)

seastar_add_test (socket
  SOURCES socket_test.cc)

seastar_add_test (sstring
  KIND BOOST
  SOURCES sstring_test.cc)

seastar_add_test (stall_detector
  SOURCES stall_detector_test.cc)

seastar_add_test (stream_reader
  SOURCES stream_reader_test.cc)

seastar_add_test (thread
  SOURCES thread_test.cc)

seastar_add_test (scheduling_group
  SOURCES scheduling_group_test.cc)

seastar_add_app_test (thread_context_switch
  SOURCES thread_context_switch_test.cc)

seastar_add_app_test (timer
  SOURCES timer_test.cc)

seastar_add_test (uname
  KIND BOOST
  SOURCES uname_test.cc)

seastar_add_test (source_location
  KIND BOOST
  SOURCES source_location_test.cc)

function(seastar_add_certgen name)
  cmake_parse_arguments(CERT
    ""
    "SUBJECT;SERVER;NAME;DOMAIN;COMMON;LOCALITY;ORG;WIDTH;STATE;COUNTRY;UNIT;EMAIL;DAYS;ALG"
    "ALG_OPTS"
    ${ARGN}
  )

  if (NOT CERT_SERVER)
    execute_process(COMMAND hostname
      RESULT_VARIABLE CERT_SERVER
    )
  endif()
  if (NOT CERT_DOMAIN)
    execute_process(COMMAND dnsdomainname
      RESULT_VARIABLE CERT_DOMAIN
    )
  endif()
  if (NOT CERT_NAME)
    set(CERT_NAME ${CERT_SERVER})
  endif()
  if (NOT CERT_COUNTRY)
    set(CERT_COUNTRY SE)
  endif()
  if (NOT CERT_STATE)
    set(CERT_STATE Stockholm)
  endif()
  if (NOT CERT_LOCALITY)
    set(CERT_LOCALITY ${CERT_STATE})
  endif()
  if (NOT CERT_ORG)
    set(CERT_ORG ${CERT_DOMAIN})
  endif()
  if (NOT CERT_UNIT)
    set(CERT_UNIT ${CERT_DOMAIN})
  endif()
  if (NOT CERT_COMMON)
    set(CERT_COMMON ${CERT_SERVER}.${CERT_DOMAIN})
  endif()
  if (NOT CERT_EMAIL)
    set(CERT_EMAIL postmaster@${CERT_DOMAIN})
  endif()
  if (NOT CERT_WIDTH)
    set(CERT_WIDTH 4096)
  endif()
  if (NOT CERT_DAYS)
    set(CERT_DAYS 3650)
  endif()
  if ((NOT CERT_ALG) AND (NOT CERT_ALG_OPTS))
    set(CERT_ALG_OPTS -pkeyopt rsa_keygen_bits:${CERT_WIDTH})
  endif()
  if (NOT CERT_ALG)
    set(CERT_ALG RSA)
  endif()

  set(CERT_PRIVKEY ${CERT_NAME}.key)
  set(CERT_REQ ${CERT_NAME}.csr)
  set(CERT_CERT ${CERT_NAME}.crt)

  set(CERT_CAPRIVKEY ca${CERT_NAME}.key)
  set(CERT_CAROOT ca${CERT_NAME}.pem)

  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cert.cfg.in"
    "${CMAKE_CURRENT_BINARY_DIR}/${CERT_NAME}.cfg"
  )

  find_program(OPENSSL openssl)

  add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${CERT_PRIVKEY}"
    COMMAND ${OPENSSL} genpkey -out ${CERT_PRIVKEY} -algorithm ${CERT_ALG} ${CERT_ALG_OPTS}
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
  )
  add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${CERT_REQ}"
    COMMAND ${OPENSSL} req -new -key ${CERT_PRIVKEY} -out ${CERT_REQ} -config ${CERT_NAME}.cfg
    DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/${CERT_PRIVKEY}"
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
  )

  add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${CERT_CAPRIVKEY}"
    COMMAND ${OPENSSL} genpkey -out ${CERT_CAPRIVKEY} -algorithm ${CERT_ALG} ${CERT_ALG_OPTS}
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
  )
  add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${CERT_CAROOT}"
    COMMAND ${OPENSSL} req -x509 -new -nodes -key ${CERT_CAPRIVKEY} -days ${CERT_DAYS} -config ${CERT_NAME}.cfg -out ${CERT_CAROOT}
    DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/${CERT_CAPRIVKEY}"
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
  )

  add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${CERT_CERT}"
    COMMAND ${OPENSSL} x509 -req -in ${CERT_REQ} -CA ${CERT_CAROOT} -CAkey ${CERT_CAPRIVKEY} -CAcreateserial -out ${CERT_CERT} -days ${CERT_DAYS}
    DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/${CERT_REQ}"  "${CMAKE_CURRENT_BINARY_DIR}/${CERT_CAROOT}"
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
  )

  add_custom_target(${name}
    DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/${CERT_CERT}"
  )
endfunction()

seastar_add_certgen(testcrt DOMAIN scylladb.org SERVER test)
seastar_add_certgen(othercrt DOMAIN apa.org SERVER other)

set (tls_certificate_files
  tls-ca-bundle.pem
)

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

add_custom_target(tls_files
  DEPENDS ${out_tls_certificate_files}
)

seastar_add_test (tls
  DEPENDS tls_files testcrt othercrt
  SOURCES tls_test.cc
  LIBRARIES Boost::filesystem
  WORKING_DIRECTORY ${Seastar_BINARY_DIR})

seastar_add_test (tuple_utils
  KIND BOOST
  SOURCES tuple_utils_test.cc)

seastar_add_test (unix_domain
  SOURCES unix_domain_test.cc)

seastar_add_test (unwind
  KIND BOOST
  SOURCES unwind_test.cc)

seastar_add_test (weak_ptr
  KIND BOOST
  SOURCES weak_ptr_test.cc)

seastar_add_test (log_buf
  SOURCES log_buf_test.cc)

seastar_add_test (exception_logging
  KIND BOOST
  SOURCES exception_logging_test.cc)

seastar_add_test (closeable
  SOURCES closeable_test.cc)

seastar_add_test (pipe
  SOURCES pipe_test.cc)
