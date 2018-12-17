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

cmake_minimum_required (VERSION 3.5)

list (APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include (Cooking OPTIONAL)

project (Seastar
  VERSION 1.0
  LANGUAGES CXX)

option (Seastar_ALLOC_FAILURE_INJECTION
  "Enable failure injection into the Seastar allocator."
  OFF)

#
# Add a dev build type.
#
# All pre-defined build modes include optimizations or debug info,
# which make them slow to build. The dev build mode is intended for
# fast build/test iteration.
#

set (CMAKE_CXX_FLAGS_DEV
  "-O1"
  CACHE
  STRING
  "Flags used by the C++ compiler during dev builds."
  FORCE)

set (CMAKE_C_FLAGS_DEV
  "-O1"
  CACHE
  STRING
  "Flags used by the C compiler during dev builds."
  FORCE)

set (CMAKE_EXE_LINKER_FLAGS_DEV
  ""
  CACHE
  STRING
  "Flags used for linking binaries during dev builds."
  FORCE)

set (CMAKE_SHARED_LINKER_FLAGS_DEV
  ""
  CACHE
  STRING
  "Flags used by the shared libraries linker during builds."
  FORCE)

mark_as_advanced (
    CMAKE_CXX_FLAGS_DEV
    CMAKE_C_FLAGS_DEV
    CMAKE_EXE_LINKER_FLAGS_DEV
    CMAKE_SHARED_LINKER_FLAGS_DEV)

set (CMAKE_BUILD_TYPE
  "${CMAKE_BUILD_TYPE}"
  CACHE
  STRING
  "Choose the type of build, options are: None Debug Release RelWithDebInfo MinSizeRel Dev."
  FORCE)

set (Seastar_ALLOC_PAGE_SIZE
  ""
  CACHE
  STRING
  "Override the Seastar allocator page size, in bytes.")

option (Seastar_APPS
  "Enable application targets."
  ON)

set (Seastar_CXX_DIALECT
  "gnu++17"
  CACHE
  STRING
  "Compile Seastar according to the named C++ standard.")

set (Seastar_CXX_FLAGS
  ""
  CACHE
  STRING
  "Semicolon-separated list of extra compilation flags for Seastar itself.")

option (Seastar_DEMOS
  "Enable demonstration targets."
  ON)

option (Seastar_DOCS
  "Enable documentation targets."
  ON)

option (Seastar_DPDK
  "Enable DPDK support."
  OFF)

option (Seastar_EXCEPTION_SCALABILITY_WORKAROUND
  "Enable a workaround for C++ exception scalability issues by overriding the definition of `dl_iterate_phdr`. Disabled for non-release builds."
  ON)

option (Seastar_EXCLUDE_APPS_FROM_ALL
  "When enabled alongside Seastar_APPS, do not build applications by default."
  OFF)

option (Seastar_EXCLUDE_DEMOS_FROM_ALL
  "When enabled alongside Seastar_DEMOS, do not build demonstrations by default."
  OFF)

option (Seastar_EXCLUDE_TESTS_FROM_ALL
  "When enabled alongside Seastar_TESTING, do not build tests by default."
  OFF)

option (Seastar_EXECUTE_ONLY_FAST_TESTS
  "Only execute tests which run quickly."
  OFF)

option (Seastar_GCC6_CONCEPTS
  "Enable compilation with gcc version 6 concepts support."
  OFF)

option (Seastar_HWLOC
  "Enable hwloc support."
  ON)

set (Seastar_JENKINS
  ""
  CACHE
  STRING
  "If non-empty, the prefix for XML files containing the results of running tests (for Jenkins).")

set (Seastar_LD_FLAGS
  ""
  CACHE
  STRING
  "Semicolon-separated list of extra linking flags for Seastar itself.")

option (Seastar_INSTALL
  "Install targets."
  ON)

option (Seastar_NUMA
  "Enable NUMA support."
  ON)

option (Seastar_STD_OPTIONAL_VARIANT_STRINGVIEW
  "Use the non-experimental versions of `optional`, `variant`, and `string_view`. Requires C++17."
  OFF)

option (Seastar_TESTING
  "Enable testing targets."
  ON)

set (Seastar_TEST_TIMEOUT
  "300"
  CACHE
  STRING
  "Maximum allowed time for a test to run, in seconds.")

# When Seastar is embedded with `add_subdirectory`, disable the non-library targets.
if (NOT (CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR))
  set (Seastar_APPS OFF)
  set (Seastar_DEMOS OFF)
  set (Seastar_DOCS OFF)
  set (Seastar_INSTALL OFF)
  set (Seastar_TESTING OFF)
endif ()

#
# Useful (non-cache) variables.
#

set (Seastar_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set (Seastar_BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR})
set (Seastar_GEN_BINARY_DIR ${Seastar_BINARY_DIR}/gen)

#
# Dependencies.
#

##
## Public dependencies.
##

find_package (Boost 1.64.0 REQUIRED
  COMPONENTS
    filesystem
    program_options
    thread
  OPTIONAL_COMPONENTS unit_test_framework)

find_package (c-ares 1.13.0 REQUIRED MODULE)
find_package (cryptopp 5.6.5 REQUIRED)
# No version information published.
find_package (dpdk)
find_package (fmt 4.0.0 REQUIRED)
find_package (lz4 1.7.3 REQUIRED)

##
## Private and private/public dependencies.
##

find_package (Concepts)
find_package (GnuTLS 3.3.26 REQUIRED)
find_package (LinuxMembarrier)
find_package (Protobuf 2.5.0 REQUIRED)
find_package (Sanitizers REQUIRED)
find_package (StdFilesystem REQUIRED)
find_package (hwloc 1.11.2)
# No version information published.
find_package (lksctp-tools REQUIRED)
# No version information published.
find_package (numactl)
find_package (ragel 6.10 REQUIRED)
find_package (rt REQUIRED)
find_package (yaml-cpp 0.5.1 REQUIRED)

#
# Code generation helpers.
#

function (seastar_generate_protobuf)
  set (one_value_args TARGET VAR IN_FILE OUT_DIR)
  cmake_parse_arguments (args "" "${one_value_args}" "" ${ARGN})
  get_filename_component (in_file_name ${args_IN_FILE} NAME_WE)
  get_filename_component (in_file_dir ${args_IN_FILE} DIRECTORY)
  set (header_out ${args_OUT_DIR}/${in_file_name}.pb.h)
  set (source_out ${args_OUT_DIR}/${in_file_name}.pb.cc)

  add_custom_command (
    DEPENDS ${args_IN_FILE}
    OUTPUT ${header_out} ${source_out}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${args_OUT_DIR}
    COMMAND ${Protobuf_PROTOC_EXECUTABLE} --cpp_out=${args_OUT_DIR} -I${in_file_dir} ${args_IN_FILE})

  add_custom_target (${args_TARGET}
    DEPENDS
      ${header_out}
      ${source_out})

  set (${args_VAR} ${header_out} ${source_out} PARENT_SCOPE)
endfunction ()

function (seastar_generate_ragel)
  set (one_value_args TARGET VAR IN_FILE OUT_FILE)
  cmake_parse_arguments (args "" "${one_value_args}" "" ${ARGN})
  get_filename_component (out_dir ${args_OUT_FILE} DIRECTORY)

  add_custom_command (
    DEPENDS ${args_IN_FILE}
    OUTPUT ${args_OUT_FILE}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${out_dir}
    COMMAND ${ragel_RAGEL_EXECUTABLE} -G2 -o ${args_OUT_FILE} ${args_IN_FILE}
    COMMAND sed -i -e "'1h;2,$$H;$$!d;g'" -re "'s/static const char _nfa[^;]*;//g'" ${args_OUT_FILE})

  add_custom_target (${args_TARGET}
    DEPENDS ${args_OUT_FILE})

  set (${args_VAR} ${args_OUT_FILE} PARENT_SCOPE)
endfunction ()

function (seastar_generate_swagger)
  set (one_value_args TARGET VAR IN_FILE OUT_FILE)
  cmake_parse_arguments (args "" "${one_value_args}" "" ${ARGN})
  get_filename_component (out_dir ${args_OUT_FILE} DIRECTORY)
  set (generator ${Seastar_SOURCE_DIR}/scripts/seastar-json2code.py)

  add_custom_command (
    DEPENDS
      ${args_IN_FILE}
      ${generator}
    OUTPUT ${args_OUT_FILE}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${out_dir}
    COMMAND ${generator} -f ${args_IN_FILE} -o ${args_OUT_FILE})

  add_custom_target (${args_TARGET}
    DEPENDS ${args_OUT_FILE})

  set (${args_VAR} ${args_OUT_FILE} PARENT_SCOPE)
endfunction ()

#
# The `seastar` library.
#

seastar_generate_ragel (
  TARGET seastar_http_request_parser
  VAR http_request_parser_file
  IN_FILE ${CMAKE_CURRENT_SOURCE_DIR}/src/http/request_parser.rl
  OUT_FILE ${Seastar_GEN_BINARY_DIR}/include/seastar/http/request_parser.hh)

seastar_generate_ragel (
  TARGET seastar_http_response_parser
  VAR http_response_parser_file
  IN_FILE ${CMAKE_CURRENT_SOURCE_DIR}/src/http/response_parser.rl
  OUT_FILE ${Seastar_GEN_BINARY_DIR}/include/seastar/http/response_parser.hh)

seastar_generate_protobuf (
  TARGET seastar_proto_metrics2
  VAR proto_metrics2_files
  IN_FILE ${CMAKE_CURRENT_SOURCE_DIR}/src/proto/metrics2.proto
  OUT_DIR ${Seastar_GEN_BINARY_DIR}/src/proto)

add_library (seastar STATIC
  ${http_request_parser_file}
  ${proto_metrics2_files}
  include/seastar/core/abort_source.hh
  include/seastar/core/alien.hh
  include/seastar/core/align.hh
  include/seastar/core/aligned_buffer.hh
  include/seastar/core/app-template.hh
  include/seastar/core/apply.hh
  include/seastar/core/array_map.hh
  include/seastar/core/bitops.hh
  include/seastar/core/bitset-iter.hh
  include/seastar/core/byteorder.hh
  include/seastar/core/cacheline.hh
  include/seastar/core/checked_ptr.hh
  include/seastar/core/chunked_fifo.hh
  include/seastar/core/circular_buffer.hh
  include/seastar/core/circular_buffer_fixed_capacity.hh
  include/seastar/core/condition-variable.hh
  include/seastar/core/deleter.hh
  include/seastar/core/distributed.hh
  include/seastar/core/do_with.hh
  include/seastar/core/dpdk_rte.hh
  include/seastar/core/enum.hh
  include/seastar/core/exception_hacks.hh
  include/seastar/core/execution_stage.hh
  include/seastar/core/expiring_fifo.hh
  include/seastar/core/fair_queue.hh
  include/seastar/core/file.hh
  include/seastar/core/fsqual.hh
  include/seastar/core/fstream.hh
  include/seastar/core/function_traits.hh
  include/seastar/core/future-util.hh
  include/seastar/core/future.hh
  include/seastar/core/gate.hh
  include/seastar/core/iostream-impl.hh
  include/seastar/core/iostream.hh
  include/seastar/core/linux-aio.hh
  include/seastar/core/lowres_clock.hh
  include/seastar/core/manual_clock.hh
  include/seastar/core/memory.hh
  include/seastar/core/metrics.hh
  include/seastar/core/metrics_api.hh
  include/seastar/core/metrics_registration.hh
  include/seastar/core/metrics_types.hh
  include/seastar/core/pipe.hh
  include/seastar/core/posix.hh
  include/seastar/core/preempt.hh
  include/seastar/core/prefetch.hh
  include/seastar/core/print.hh
  include/seastar/core/prometheus.hh
  include/seastar/core/queue.hh
  include/seastar/core/ragel.hh
  include/seastar/core/reactor.hh
  include/seastar/core/report_exception.hh
  include/seastar/core/resource.hh
  include/seastar/core/rwlock.hh
  include/seastar/core/scattered_message.hh
  include/seastar/core/scheduling.hh
  include/seastar/core/scollectd.hh
  include/seastar/core/scollectd_api.hh
  include/seastar/core/seastar.hh
  include/seastar/core/semaphore.hh
  include/seastar/core/sharded.hh
  include/seastar/core/shared_future.hh
  include/seastar/core/shared_mutex.hh
  include/seastar/core/shared_ptr.hh
  include/seastar/core/shared_ptr_debug_helper.hh
  include/seastar/core/shared_ptr_incomplete.hh
  include/seastar/core/simple-stream.hh
  include/seastar/core/slab.hh
  include/seastar/core/sleep.hh
  include/seastar/core/sstring.hh
  include/seastar/core/stall_sampler.hh
  include/seastar/core/stream.hh
  include/seastar/core/systemwide_memory_barrier.hh
  include/seastar/core/task.hh
  include/seastar/core/temporary_buffer.hh
  include/seastar/core/thread.hh
  include/seastar/core/thread_cputime_clock.hh
  include/seastar/core/thread_impl.hh
  include/seastar/core/timer-set.hh
  include/seastar/core/timer.hh
  include/seastar/core/transfer.hh
  include/seastar/core/unaligned.hh
  include/seastar/core/units.hh
  include/seastar/core/vector-data-sink.hh
  include/seastar/core/weak_ptr.hh
  include/seastar/http/api_docs.hh
  include/seastar/http/common.hh
  include/seastar/http/exception.hh
  include/seastar/http/file_handler.hh
  include/seastar/http/function_handlers.hh
  include/seastar/http/handlers.hh
  include/seastar/http/httpd.hh
  include/seastar/http/json_path.hh
  include/seastar/http/matcher.hh
  include/seastar/http/matchrules.hh
  include/seastar/http/mime_types.hh
  include/seastar/http/reply.hh
  include/seastar/http/request.hh
  include/seastar/http/routes.hh
  include/seastar/http/transformers.hh
  include/seastar/json/formatter.hh
  include/seastar/json/json_elements.hh
  include/seastar/net/api.hh
  include/seastar/net/arp.hh
  include/seastar/net/byteorder.hh
  include/seastar/net/config.hh
  include/seastar/net/const.hh
  include/seastar/net/dhcp.hh
  include/seastar/net/dns.hh
  include/seastar/net/dpdk.hh
  include/seastar/net/ethernet.hh
  include/seastar/net/inet_address.hh
  include/seastar/net/ip.hh
  include/seastar/net/ip_checksum.hh
  include/seastar/net/native-stack.hh
  include/seastar/net/net.hh
  include/seastar/net/packet-data-source.hh
  include/seastar/net/packet-util.hh
  include/seastar/net/packet.hh
  include/seastar/net/posix-stack.hh
  include/seastar/net/proxy.hh
  include/seastar/net/socket_defs.hh
  include/seastar/net/stack.hh
  include/seastar/net/tcp-stack.hh
  include/seastar/net/tcp.hh
  include/seastar/net/tls.hh
  include/seastar/net/toeplitz.hh
  include/seastar/net/udp.hh
  include/seastar/net/virtio-interface.hh
  include/seastar/net/virtio.hh
  include/seastar/rpc/lz4_compressor.hh
  include/seastar/rpc/multi_algo_compressor_factory.hh
  include/seastar/rpc/rpc.hh
  include/seastar/rpc/rpc_impl.hh
  include/seastar/rpc/rpc_types.hh
  include/seastar/util/alloc_failure_injector.hh
  include/seastar/util/backtrace.hh
  include/seastar/util/bool_class.hh
  include/seastar/util/conversions.hh
  include/seastar/util/defer.hh
  include/seastar/util/eclipse.hh
  include/seastar/util/function_input_iterator.hh
  include/seastar/util/gcc6-concepts.hh
  include/seastar/util/indirect.hh
  include/seastar/util/is_smart_ptr.hh
  include/seastar/util/lazy.hh
  include/seastar/util/log-cli.hh
  include/seastar/util/log.hh
  include/seastar/util/noncopyable_function.hh
  include/seastar/util/optimized_optional.hh
  include/seastar/util/print_safe.hh
  include/seastar/util/program-options.hh
  include/seastar/util/read_first_line.hh
  include/seastar/util/reference_wrapper.hh
  include/seastar/util/spinlock.hh
  include/seastar/util/std-compat.hh
  include/seastar/util/transform_iterator.hh
  include/seastar/util/tuple_utils.hh
  include/seastar/util/variant_utils.hh
  src/core/alien.cc
  src/core/app-template.cc
  src/core/dpdk_rte.cc
  src/core/exception_hacks.cc
  src/core/execution_stage.cc
  src/core/file-impl.hh
  src/core/fsqual.cc
  src/core/fstream.cc
  src/core/future-util.cc
  src/core/linux-aio.cc
  src/core/memory.cc
  src/core/metrics.cc
  src/core/posix.cc
  src/core/prometheus.cc
  src/core/reactor.cc
  src/core/resource.cc
  src/core/sharded.cc
  src/core/scollectd.cc
  src/core/scollectd-impl.hh
  src/core/systemwide_memory_barrier.cc
  src/core/thread.cc
  src/core/vla.hh
  src/http/api_docs.cc
  src/http/common.cc
  src/http/file_handler.cc
  src/http/httpd.cc
  src/http/json_path.cc
  src/http/matcher.cc
  src/http/mime_types.cc
  src/http/reply.cc
  src/http/routes.cc
  src/http/transformers.cc
  src/json/formatter.cc
  src/json/json_elements.cc
  src/net/arp.cc
  src/net/config.cc
  src/net/dhcp.cc
  src/net/dns.cc
  src/net/dpdk.cc
  src/net/ethernet.cc
  src/net/inet_address.cc
  src/net/ip.cc
  src/net/ip_checksum.cc
  src/net/native-stack-impl.hh
  src/net/native-stack.cc
  src/net/net.cc
  src/net/packet.cc
  src/net/posix-stack.cc
  src/net/proxy.cc
  src/net/stack.cc
  src/net/tcp.cc
  src/net/tls.cc
  src/net/udp.cc
  src/net/virtio.cc
  src/rpc/lz4_compressor.cc
  src/rpc/rpc.cc
  src/util/alloc_failure_injector.cc
  src/util/backtrace.cc
  src/util/conversions.cc
  src/util/log.cc
  src/util/program-options.cc
  src/util/read_first_line.cc)

add_library (Seastar::seastar ALIAS seastar)

add_dependencies (seastar
  seastar_http_request_parser
  seastar_http_response_parser
  seastar_proto_metrics2)

target_include_directories (seastar
  PUBLIC
    $<INSTALL_INTERFACE:include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${Seastar_GEN_BINARY_DIR}/include>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${Seastar_GEN_BINARY_DIR}/src)

target_link_libraries (seastar
  PUBLIC
    Boost::boost
    Boost::program_options
    Boost::thread
    c-ares::c-ares
    cryptopp::cryptopp
    fmt::fmt
    lz4::lz4
  PRIVATE
    ${CMAKE_DL_LIBS}
    Boost::filesystem
    GnuTLS::gnutls
    StdFilesystem::filesystem
    lksctp-tools::lksctp-tools
    protobuf::libprotobuf
    rt::rt
    yaml-cpp::yaml-cpp
    debug Sanitizers::address
    debug Sanitizers::undefined_behavior)

#
# We want asserts enabled on all modes, but cmake defaults to passing
# -DNDEBUG in some modes. We add -UNDEBUG to our private options to
# reenable it. To force asserts off pass -DNDEBUG in
# Seastar_CXX_FLAGS.
#

target_compile_options (seastar
  PUBLIC -std=${Seastar_CXX_DIALECT}
  PRIVATE -UNDEBUG)

if (Seastar_GCC6_CONCEPTS OR Concepts_FOUND)
  target_compile_definitions (seastar
    PUBLIC SEASTAR_HAVE_GCC6_CONCEPTS)

  target_link_libraries (seastar
    PUBLIC Concepts::concepts)
endif ()

if (LinuxMembarrier_FOUND)
  target_compile_definitions (seastar
    PRIVATE SEASTAR_HAS_MEMBARRIER)

  target_link_libraries (seastar
    PRIVATE LinuxMembarrier::membarrier)
endif ()

if (Seastar_ALLOC_FAILURE_INJECTION)
  target_compile_definitions (seastar
    PUBLIC SEASTAR_ENABLE_ALLOC_FAILURE_INJECTION)
endif ()

if (Seastar_STD_OPTIONAL_VARIANT_STRINGVIEW)
  target_compile_definitions (seastar
    PUBLIC SEASTAR_USE_STD_OPTIONAL_VARIANT_STRINGVIEW)
endif ()

if (Sanitizers_FIBER_SUPPORT)
  target_compile_definitions (seastar
    PRIVATE SEASTAR_HAVE_ASAN_FIBER_SUPPORT)
endif ()

if (Seastar_ALLOC_PAGE_SIZE)
  target_compile_definitions (seastar
    PUBLIC SEASTAR_OVERRIDE_ALLOCATOR_PAGE_SIZE=${Seastar_ALLOC_PAGE_SIZE})
endif ()

if (Seastar_CXX_FLAGS)
  target_compile_options (seastar
    PRIVATE ${Seastar_CXX_FLAGS})
endif ()

if (Seastar_DPDK)
  if (NOT dpdk_FOUND)
    message (FATAL_ERROR "dpdk support is enabled but it is not available!")
  endif ()

  target_compile_definitions (seastar
    PUBLIC SEASTAR_HAVE_DPDK)

  target_link_libraries (seastar
    PUBLIC dpdk::dpdk)
endif ()

if ((CMAKE_BUILD_TYPE STREQUAL "Debug") OR (NOT Seastar_EXCEPTION_SCALABILITY_WORKAROUND))
  target_compile_definitions (seastar
    PRIVATE SEASTAR_NO_EXCEPTION_HACK)
endif ()

if (Seastar_HWLOC)
  if (NOT hwloc_FOUND)
    message (FATAL_ERROR "`hwloc` support is enabled but it is not available!")
  endif ()

  target_compile_definitions (seastar
    PRIVATE SEASTAR_HAVE_HWLOC)

  target_link_libraries (seastar
    PRIVATE hwloc::hwloc)
endif ()

if (Seastar_LD_FLAGS)
  # In newer versions of CMake, there is `target_link_options`.
  target_link_libraries (seastar
    PRIVATE ${Seastar_LD_FLAGS})
endif ()

if (Seastar_NUMA)
  if (NOT numactl_FOUND)
    message (FATAL_ERROR "NUMA support is enabled but `numactl` is not available!")
  endif ()

  target_compile_definitions (seastar
    PRIVATE SEASTAR_HAVE_NUMA)

  target_link_libraries (seastar
    PRIVATE numactl::numactl)
endif ()

if (lz4_HAVE_COMPRESS_DEFAULT)
  target_compile_definitions (seastar
    PRIVATE SEASTAR_HAVE_LZ4_COMPRESS_DEFAULT)
endif ()

if (CMAKE_BUILD_TYPE STREQUAL "Debug")
  target_compile_definitions (seastar
    PUBLIC
      SEASTAR_DEBUG
      SEASTAR_DEBUG_SHARED_PTR
      SEASTAR_ASAN_ENABLED
      SEASTAR_THREAD_STACK_GUARDS
    PRIVATE SEASTAR_DEFAULT_ALLOCATOR)
endif ()

#
# The testing library.
#

if (Seastar_INSTALL OR Seastar_TESTING)
  add_library (seastar_testing
    include/seastar/testing/entry_point.hh
    include/seastar/testing/exchanger.hh
    include/seastar/testing/seastar_test.hh
    include/seastar/testing/test_case.hh
    include/seastar/testing/test_runner.hh
    include/seastar/testing/thread_test_case.hh
    src/testing/entry_point.cc
    src/testing/seastar_test.cc
    src/testing/test_runner.cc)

  add_library (Seastar::seastar_testing ALIAS seastar_testing)

  target_compile_definitions (seastar_testing
    PUBLIC BOOST_TEST_DYN_LINK)

  target_link_libraries (seastar_testing
    PUBLIC
      Boost::unit_test_framework
      seastar)
endif ()

#
# The tests themselves.
#

if (Seastar_TESTING)
  enable_testing ()

  if (Seastar_EXCLUDE_TESTS_FROM_ALL)
    set (exclude EXCLUDE_FROM_ALL)
  else ()
    set (exclude "")
  endif ()

  add_subdirectory (tests ${exclude})
endif ()

#
# Demonstrations.
#

if (Seastar_DEMOS)
  if (Seastar_EXCLUDE_DEMOS_FROM_ALL)
    set (exclude EXCLUDE_FROM_ALL)
  else ()
    set (exclude "")
  endif ()

  add_subdirectory (demos ${exclude})
endif ()

#
# Documentation.
#

if (Seastar_DOCS)
  add_subdirectory (doc)
endif ()

#
# Applications.
#

if (Seastar_APPS)
  if (Seastar_EXCLUDE_APPS_FROM_ALL)
    set (exclude EXCLUDE_FROM_ALL)
  else ()
    set (exclude "")
  endif ()

  add_subdirectory (apps ${exclude})
endif ()

#
# Installation and export.
#

if (Seastar_INSTALL)
  #
  # pkg-config generation.
  #
  # Note that unlike the CMake "config module", this description is not relocatable because
  # some dependencies do not natively support pkg-config.
  #

  # Necessary here for pkg-config.
  include (GNUInstallDirs)

  if (NOT ("${CMAKE_BUILD_TYPE}" STREQUAL "Debug"))
    set (Sanitizers_PKGCONFIG_COMPILER_OPTIONS "")
  else ()
    set (Sanitizers_PKGCONFIG_COMPILER_OPTIONS ${Sanitizers_COMPILER_OPTIONS})
  endif ()

  configure_file (
    ${CMAKE_CURRENT_SOURCE_DIR}/pkgconfig/seastar.pc.in
    ${CMAKE_CURRENT_BINARY_DIR}/pkgconfig/seastar.pc.in
    @ONLY)

  configure_file (
    ${CMAKE_CURRENT_SOURCE_DIR}/pkgconfig/seastar-testing.pc.in
    ${CMAKE_CURRENT_BINARY_DIR}/pkgconfig/seastar-testing.pc.in
    @ONLY)

  file (GENERATE
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/seastar.pc
    INPUT ${CMAKE_CURRENT_BINARY_DIR}/pkgconfig/seastar.pc.in)

  file (GENERATE
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/seastar-testing.pc
    INPUT ${CMAKE_CURRENT_BINARY_DIR}/pkgconfig/seastar-testing.pc.in)

  include (CMakePackageConfigHelpers)
  set (install_cmakedir ${CMAKE_INSTALL_LIBDIR}/cmake/Seastar)

  install (
    DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

  install (
    DIRECTORY ${Seastar_GEN_BINARY_DIR}/include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

  install (
    PROGRAMS ${CMAKE_CURRENT_SOURCE_DIR}/scripts/seastar-json2code.py
    DESTINATION ${CMAKE_INSTALL_BINDIR})

  install (
    TARGETS
      seastar
      seastar_testing
    EXPORT seastar-export
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

  install (
    EXPORT seastar-export
    FILE SeastarTargets.cmake
    NAMESPACE Seastar::
    DESTINATION ${install_cmakedir})

  write_basic_package_version_file (
    ${CMAKE_CURRENT_BINARY_DIR}/SeastarConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY ExactVersion)

  configure_package_config_file (
    ${CMAKE_CURRENT_LIST_DIR}/cmake/SeastarConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/SeastarConfig.cmake
    INSTALL_DESTINATION ${install_cmakedir})

  install (
    FILES
      ${CMAKE_CURRENT_BINARY_DIR}/SeastarConfig.cmake
      ${CMAKE_CURRENT_BINARY_DIR}/SeastarConfigVersion.cmake
    DESTINATION ${install_cmakedir})

  install (
    FILES
      ${CMAKE_CURRENT_SOURCE_DIR}/cmake/FindBoost.cmake
      ${CMAKE_CURRENT_SOURCE_DIR}/cmake/FindConcepts.cmake
      ${CMAKE_CURRENT_SOURCE_DIR}/cmake/FindGnuTLS.cmake
      ${CMAKE_CURRENT_SOURCE_DIR}/cmake/FindLinuxMembarrier.cmake
      ${CMAKE_CURRENT_SOURCE_DIR}/cmake/FindProtobuf.cmake
      ${CMAKE_CURRENT_SOURCE_DIR}/cmake/FindSanitizers.cmake
      ${CMAKE_CURRENT_SOURCE_DIR}/cmake/FindStdFilesystem.cmake
      ${CMAKE_CURRENT_SOURCE_DIR}/cmake/Findc-ares.cmake
      ${CMAKE_CURRENT_SOURCE_DIR}/cmake/Findcryptopp.cmake
      ${CMAKE_CURRENT_SOURCE_DIR}/cmake/Finddpdk.cmake
      ${CMAKE_CURRENT_SOURCE_DIR}/cmake/Findhwloc.cmake
      ${CMAKE_CURRENT_SOURCE_DIR}/cmake/Findlksctp-tools.cmake
      ${CMAKE_CURRENT_SOURCE_DIR}/cmake/Findlz4.cmake
      ${CMAKE_CURRENT_SOURCE_DIR}/cmake/Findnumactl.cmake
      ${CMAKE_CURRENT_SOURCE_DIR}/cmake/Findragel.cmake
      ${CMAKE_CURRENT_SOURCE_DIR}/cmake/Findrt.cmake
      ${CMAKE_CURRENT_SOURCE_DIR}/cmake/Findyaml-cpp.cmake
    DESTINATION ${install_cmakedir})

  install (
    DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/cmake/code_tests
    DESTINATION ${install_cmakedir})

  install (
    FILES
      ${CMAKE_CURRENT_BINARY_DIR}/seastar.pc
      ${CMAKE_CURRENT_BINARY_DIR}/seastar-testing.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

  #
  # Export targets from the build tree for the user package registry.
  #

  export (
    EXPORT seastar-export
    FILE ${CMAKE_CURRENT_BINARY_DIR}/SeastarTargets.cmake
    NAMESPACE Seastar::)

  export (PACKAGE Seastar)

  #
  # Packaging.
  #

  set (CPACK_PACKAGE_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
  set (CPACK_PACKAGE_VERSION_MINOR ${PROJECT_VERSION_MINOR})
  set (CPACK_PACKAGE_VERSION_PATCH ${PROJECT_VERSION_PATCH})

  include (CPack)
endif ()
