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

cmake_minimum_required (VERSION 3.13)

list (APPEND CMAKE_MODULE_PATH
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake
  ${CMAKE_CURRENT_BINARY_DIR})

include (Cooking OPTIONAL)

# This variable impacts the way DPDK is configured by cmake-cooking (if DPDK is enabled), so its definition needs to
# come before PROJECT.
set (Seastar_DPDK_MACHINE
  "native"
  CACHE
  STRING
  "Configure DPDK for this processor architecture (if `Seastar_DPDK` is enabled). A DPDK code name abbreviation (e.g., ivb)")

project (Seastar
  VERSION 1.0
  LANGUAGES CXX)

cmake_policy (SET CMP0090 NEW)
if (POLICY CMP0135)
  cmake_policy (SET CMP0135 NEW)
endif ()

# generic boolean values passed as string, potentially from configure.py
set (True_STRING_VALUES "ON" "yes" "Yes" "YES" "true" "True" "TRUE")
set (False_STRING_VALUES "OFF" "no" "No" "NO" "false" "False" "FALSE")
set (Default_STRING_VALUES "DEFAULT" "default" "Default")

set (Seastar_ALLOC_FAILURE_INJECTION
  "DEFAULT"
  CACHE
  STRING
  "Enable failure injection into the Seastar allocator. Can be ON, OFF or DEFAULT (which enables it for Dev mode)")

option (Seastar_TASK_BACKTRACE
  "Collect backtrace at deferring points."
  OFF)

option (Seastar_DEBUG_ALLOCATIONS
  "For now just writes 0xab to newly allocated memory"
  OFF)

option (Seastar_SSTRING
  "Use seastar's own string implementation"
  ON)

set (Seastar_API_LEVEL
  "6"
  CACHE
  STRING
  "Seastar compatibility API level (2=server_socket::accept() returns accept_result, 3=make_file_output_stream(), make_file_data_sink() returns future<...>, 4=when_all_succeed returns future<std::tuple<>>, 5=future<T>::get() returns T&&), 6=future is not variadic")

set_property (CACHE Seastar_API_LEVEL
  PROPERTY
  STRINGS 2 3 4 5 6)

set (Seastar_SCHEDULING_GROUPS_COUNT
  "16"
  CACHE
  STRING
  "A positive number to set Seastar's reactor number of allowed different scheduling groups.")

if (NOT Seastar_SCHEDULING_GROUPS_COUNT MATCHES "^[1-9][0-9]*")
  message(FATAL_ERROR "Seastar_SCHEDULING_GROUPS_COUNT must be a positive number (${Seastar_SCHEDULING_GROUPS_COUNT})")
endif()

#
# Add a dev build type.
#
# All pre-defined build modes include optimizations or debug info,
# which make them slow to build. The dev build mode is intended for
# fast build/test iteration.
#

if (CMAKE_CXX_COMPILER_ID MATCHES Clang)
  set (CMAKE_CXX_FLAGS_DEV_OPT_LEVEL "-O2")
else ()
  set (CMAKE_CXX_FLAGS_DEV_OPT_LEVEL "-O1")
endif ()

set (CMAKE_CXX_FLAGS_DEV
  "${CMAKE_CXX_FLAGS_DEV_OPT_LEVEL}"
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

set (CMAKE_CXX_FLAGS_SANITIZE
  "-Os -g"
  CACHE
  STRING
  "Flags used by the C++ compiler during sanitize builds."
  FORCE)

set (CMAKE_CXX_STANDARD
  "20"
  CACHE
  STRING
  "C++ standard to build with.")

set (CMAKE_C_FLAGS_SANITIZE
  "-Os -g"
  CACHE
  STRING
  "Flags used by the C compiler during sanitize builds."
  FORCE)

set (CMAKE_EXE_LINKER_FLAGS_SANITIZE
  ""
  CACHE
  STRING
  "Flags used for linking binaries during sanitize builds."
  FORCE)

set (CMAKE_SHARED_LINKER_FLAGS_SANITIZE
  ""
  CACHE
  STRING
  "Flags used by the shared libraries linker during sanitize builds."
  FORCE)

mark_as_advanced (
    CMAKE_CXX_FLAGS_SANITIZE
    CMAKE_C_FLAGS_SANITIZE
    CMAKE_EXE_LINKER_FLAGS_SANITIZE
    CMAKE_SHARED_LINKER_FLAGS_SANITIZE)

set (CMAKE_BUILD_TYPE
  "${CMAKE_BUILD_TYPE}"
  CACHE
  STRING
  "Choose the type of build, options are: None Debug Release RelWithDebInfo MinSizeRel Dev Sanitize."
  FORCE)

if (NOT CMAKE_BUILD_TYPE)
  set (CMAKE_BUILD_TYPE "Release")
endif ()

set (Seastar_ALLOC_PAGE_SIZE
  ""
  CACHE
  STRING
  "Override the Seastar allocator page size, in bytes.")

function (set_option_if_package_is_found option_name package_name)
  # if the package is found, set the option on behalf of user unless it is
  # explicitly specified,
  if (DEFINED ${option_name})
    return ()
  endif ()
  if (${package_name}_FOUND)
    set (${option_name} ON CACHE BOOL "")
  endif ()
endfunction ()

# When Seastar is a top-level project, enable the non-library targets by default.
# If it is embedded with `add_subdirectory`, disable them.
if (CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
  set (Seastar_MASTER_PROJECT ON)
else ()
  set (Seastar_MASTER_PROJECT OFF)
endif ()

option (Seastar_APPS
  "Enable application targets."
  ${Seastar_MASTER_PROJECT})

set (Seastar_CXX_FLAGS
  ""
  CACHE
  STRING
  "Semicolon-separated list of extra compilation flags for Seastar itself.")

option (Seastar_DEMOS
  "Enable demonstration targets."
  ${Seastar_MASTER_PROJECT})

option (Seastar_DOCS
  "Enable documentation targets."
  ${Seastar_MASTER_PROJECT})

option (Seastar_DPDK
  "Enable DPDK support."
  OFF)

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

option (Seastar_HWLOC
  "Enable hwloc support."
  ON)

if (DEFINED Seastar_IO_URING)
  option (Seastar_IO_URING
    "Enable io_uring support."
    ON)
endif ()

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
  ${Seastar_MASTER_PROJECT})

option (Seastar_NUMA
  "Enable NUMA support."
  ON)

option (Seastar_TESTING
  "Enable testing targets."
  ${Seastar_MASTER_PROJECT})

include (CMakeDependentOption)
cmake_dependent_option (Seastar_ENABLE_TESTS_ACCESSING_INTERNET
  "Enable tests accessing internet." ON
  "Seastar_TESTING" OFF)

option (Seastar_COMPRESS_DEBUG
  "Compress debug info."
  ON)

option (Seastar_SPLIT_DWARF
  "Use split dwarf."
  OFF)

option (Seastar_HEAP_PROFILING
    "Enable heap profiling. No effect when Seastar is compiled with the default allocator."
    OFF)

option (Seastar_DEFERRED_ACTION_REQUIRE_NOEXCEPT
    "Enable noexcept requirement for deferred actions."
    ON)

set (Seastar_TEST_TIMEOUT
  "300"
  CACHE
  STRING
  "Maximum allowed time for a test to run, in seconds.")

option (BUILD_SHARED_LIBS
  "Build seastar library as shared libraries instead of static"
  OFF)
# We set the following environment variables
# * ASAN_OPTIONS=disable_coredump=0:abort_on_error=1:detect_stack_use_after_return=1:verify_asan_link_order=0
#   By default asan disables core dumps because they used to be
#   huge. This is no longer the case since the shadow memory is
#   excluded, so it is safe to enable them.
#   Also, by default, to make sure it works as expected, asan
#   verifies if the asan dynamic runtime is the first DSO in the
#   initial library list by calling dl_iterate_phdr(3). But
#   Seastar provides this symbol, and its implementation references
#   some static variables. So if Seastar is built as a shared
#   library, this causes a chicken and egg problem. In other words,
#   ASan is loaded first, and it tries to reference a symbol which
#   is in turn provided by another shared library which is not
#   necessariy ready its static variables yet. And this leads leads
#   to a segfault. So, we have to disable this check when
#   "BUILD_SHARED_LIBS" is enabled.
# * UBSAN_OPTIONS=halt_on_error=1:abort_on_error=1
#   Fail the test if any undefined behavior is found and use abort
#   instead of exit. Using abort is what causes core dumps to be
#   produced.
# * BOOST_TEST_CATCH_SYSTEM_ERRORS=no
#   Normally the boost test library handles SIGABRT and prevents core
#   dumps from being produced.
# This works great with clang and gcc 10.2, but unfortunately not any
# previous gcc.
set (Seastar_ASAN_OPTIONS "disable_coredump=0:abort_on_error=1")
if ((NOT (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")) OR
    (CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 10.2))
  string (APPEND Seastar_ASAN_OPTIONS ":detect_stack_use_after_return=1")
endif ()
if (BUILD_SHARED_LIBS)
  string (APPEND Seastar_ASAN_OPTIONS ":verify_asan_link_order=0")
endif ()


set (Seastar_TEST_ENVIRONMENT
  "ASAN_OPTIONS=${Seastar_ASAN_OPTIONS};UBSAN_OPTIONS=halt_on_error=1:abort_on_error=1;BOOST_TEST_CATCH_SYSTEM_ERRORS=no"
  CACHE
  STRING
  "Environment variables for running tests")

option (Seastar_UNUSED_RESULT_ERROR
  "Make [[nodiscard]] violations an error (instead of a warning)."
  OFF)

set (Seastar_STACK_GUARDS
  "DEFAULT"
  CACHE
  STRING
  "Enable stack guards. Can be ON, OFF or DEFAULT (which enables it for non release builds)")

set (Seastar_SANITIZE
  "DEFAULT"
  CACHE
  STRING
  "Enable ASAN and UBSAN. Can be ON, OFF or DEFAULT (which enables it for Debug and Sanitize)")

set (Seastar_DEBUG_SHARED_PTR
  "DEFAULT"
  CACHE
  STRING
  "Enable shared_ptr debugging. Can be ON, OFF or DEFAULT (which enables it for Debug and Sanitize)")

#
# Useful (non-cache) variables.
#

set (Seastar_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set (Seastar_BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR})
set (Seastar_GEN_BINARY_DIR ${Seastar_BINARY_DIR}/gen)

#
# Dependencies.
#

include (SeastarDependencies)
seastar_find_dependencies ()

# Private build dependencies not visible to consumers
find_package (ragel 6.10 REQUIRED)
find_package (Threads REQUIRED)
find_package (PthreadSetName REQUIRED)
find_package (Valgrind REQUIRED)

#
# Code generation helpers.
#

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
  set (one_value_args TARGET VAR IN_FILE OUT_DIR)
  cmake_parse_arguments (args "" "${one_value_args}" "" ${ARGN})
  get_filename_component (in_file_name ${args_IN_FILE} NAME)
  set (generator ${Seastar_SOURCE_DIR}/scripts/seastar-json2code.py)
  set (header_out ${args_OUT_DIR}/${in_file_name}.hh)
  set (source_out ${args_OUT_DIR}/${in_file_name}.cc)

  add_custom_command (
    DEPENDS
      ${args_IN_FILE}
      ${generator}
    OUTPUT ${header_out} ${source_out}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${args_OUT_DIR}
    COMMAND ${generator} --create-cc -f ${args_IN_FILE} -o ${header_out})

  add_custom_target (${args_TARGET}
    DEPENDS
      ${header_out}
      ${source_out})

  set (${args_VAR} ${header_out} ${source_out} PARENT_SCOPE)
endfunction ()

#
# The `seastar` library.
#

seastar_generate_ragel (
  TARGET seastar_http_chunk_parsers
  VAR http_chunk_parsers_file
  IN_FILE ${CMAKE_CURRENT_SOURCE_DIR}/src/http/chunk_parsers.rl
  OUT_FILE ${Seastar_GEN_BINARY_DIR}/include/seastar/http/chunk_parsers.hh)

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

if (Seastar_DPDK)
  set (seastar_dpdk_obj seastar-dpdk.o)
endif ()

add_library (seastar
  ${http_chunk_parsers_file}
  ${http_request_parser_file}
  ${seastar_dpdk_obj}
  include/seastar/core/abort_source.hh
  include/seastar/core/alien.hh
  include/seastar/core/align.hh
  include/seastar/core/aligned_buffer.hh
  include/seastar/core/app-template.hh
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
  include/seastar/core/file-types.hh
  include/seastar/core/fsqual.hh
  include/seastar/core/fstream.hh
  include/seastar/core/function_traits.hh
  include/seastar/core/future-util.hh
  include/seastar/core/future.hh
  include/seastar/core/gate.hh
  include/seastar/core/iostream-impl.hh
  include/seastar/core/iostream.hh
  include/seastar/util/later.hh
  include/seastar/core/layered_file.hh
  include/seastar/core/linux-aio.hh
  include/seastar/core/loop.hh
  include/seastar/core/lowres_clock.hh
  include/seastar/core/manual_clock.hh
  include/seastar/core/map_reduce.hh
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
  include/seastar/core/timed_out_error.hh
  include/seastar/core/timer-set.hh
  include/seastar/core/timer.hh
  include/seastar/core/transfer.hh
  include/seastar/core/unaligned.hh
  include/seastar/core/units.hh
  include/seastar/core/vector-data-sink.hh
  include/seastar/core/weak_ptr.hh
  include/seastar/core/when_all.hh
  include/seastar/core/with_scheduling_group.hh
  include/seastar/core/with_timeout.hh
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
  include/seastar/http/short_streams.hh
  include/seastar/http/transformers.hh
  include/seastar/http/client.hh
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
  include/seastar/net/unix_address.hh
  include/seastar/net/virtio-interface.hh
  include/seastar/net/virtio.hh
  include/seastar/rpc/lz4_compressor.hh
  include/seastar/rpc/lz4_fragmented_compressor.hh
  include/seastar/rpc/multi_algo_compressor_factory.hh
  include/seastar/rpc/rpc.hh
  include/seastar/rpc/rpc_impl.hh
  include/seastar/rpc/rpc_types.hh
  include/seastar/util/alloc_failure_injector.hh
  include/seastar/util/backtrace.hh
  include/seastar/util/concepts.hh
  include/seastar/util/bool_class.hh
  include/seastar/util/conversions.hh
  include/seastar/util/defer.hh
  include/seastar/util/eclipse.hh
  include/seastar/util/function_input_iterator.hh
  include/seastar/util/indirect.hh
  include/seastar/util/is_smart_ptr.hh
  include/seastar/util/lazy.hh
  include/seastar/util/log-cli.hh
  include/seastar/util/log-impl.hh
  include/seastar/util/log.hh
  include/seastar/util/noncopyable_function.hh
  include/seastar/util/optimized_optional.hh
  include/seastar/util/print_safe.hh
  include/seastar/util/process.hh
  include/seastar/util/program-options.hh
  include/seastar/util/read_first_line.hh
  include/seastar/util/reference_wrapper.hh
  include/seastar/util/spinlock.hh
  include/seastar/util/std-compat.hh
  include/seastar/util/transform_iterator.hh
  include/seastar/util/tuple_utils.hh
  include/seastar/util/variant_utils.hh
  include/seastar/util/closeable.hh
  include/seastar/util/source_location-compat.hh
  include/seastar/util/short_streams.hh
  include/seastar/websocket/server.hh
  src/core/alien.cc
  src/core/file.cc
  src/core/fair_queue.cc
  src/core/reactor_backend.cc
  src/core/thread_pool.cc
  src/core/app-template.cc
  src/core/dpdk_rte.cc
  src/core/exception_hacks.cc
  src/core/execution_stage.cc
  src/core/file-impl.hh
  src/core/fsnotify.cc
  src/core/fsqual.cc
  src/core/fstream.cc
  src/core/future.cc
  src/core/future-util.cc
  src/core/linux-aio.cc
  src/core/memory.cc
  src/core/metrics.cc
  src/core/on_internal_error.cc
  src/core/posix.cc
  src/core/prometheus.cc
  src/core/program_options.cc
  src/core/reactor.cc
  src/core/resource.cc
  src/core/sharded.cc
  src/core/scollectd.cc
  src/core/scollectd-impl.hh
  src/core/systemwide_memory_barrier.cc
  src/core/smp.cc
  src/core/sstring.cc
  src/core/thread.cc
  src/core/uname.cc
  src/core/vla.hh
  src/core/io_queue.cc
  src/core/semaphore.cc
  src/core/condition-variable.cc
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
  src/http/url.cc
  src/http/client.cc
  src/http/request.cc
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
  src/net/socket_address.cc
  src/net/stack.cc
  src/net/tcp.cc
  src/net/tls.cc
  src/net/udp.cc
  src/net/unix_address.cc
  src/net/virtio.cc
  src/rpc/lz4_compressor.cc
  src/rpc/lz4_fragmented_compressor.cc
  src/rpc/rpc.cc
  src/util/alloc_failure_injector.cc
  src/util/backtrace.cc
  src/util/conversions.cc
  src/util/exceptions.cc
  src/util/file.cc
  src/util/log.cc
  src/util/process.cc
  src/util/program-options.cc
  src/util/read_first_line.cc
  src/util/tmp_file.cc
  src/util/short_streams.cc
  src/websocket/server.cc
  )

add_library (Seastar::seastar ALIAS seastar)

add_dependencies (seastar
  seastar_http_chunk_parsers
  seastar_http_request_parser
  seastar_http_response_parser)

target_include_directories (seastar
  PUBLIC
    $<INSTALL_INTERFACE:include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${Seastar_GEN_BINARY_DIR}/include>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)

set (Seastar_PRIVATE_CXX_FLAGS
  -UNDEBUG
  -Wall
  -Werror
  -Wno-array-bounds # Disabled because of https://gcc.gnu.org/bugzilla/show_bug.cgi?id=93437
  -Wno-error=deprecated-declarations)
if (NOT BUILD_SHARED_LIBS)
  list (APPEND Seastar_PRIVATE_CXX_FLAGS -fvisibility=hidden)
endif ()

if (Seastar_COMPRESS_DEBUG)
  # -gz doesn't imply -g, so it is safe to add it regardless of debug
  # info being enabled.
  list (APPEND Seastar_PRIVATE_CXX_FLAGS -gz)
endif()

target_link_libraries (seastar
  PUBLIC
    Boost::boost
    Boost::program_options
    Boost::thread
    c-ares::cares
    cryptopp::cryptopp
    fmt::fmt
    lz4::lz4
    SourceLocation::source_location
  PRIVATE
    ${CMAKE_DL_LIBS}
    GnuTLS::gnutls
    StdAtomic::atomic
    lksctp-tools::lksctp-tools
    rt::rt
    yaml-cpp::yaml-cpp
    "$<BUILD_INTERFACE:Valgrind::valgrind>"
    Threads::Threads)

set (Seastar_SANITIZE_MODES "Debug" "Sanitize")
if ((Seastar_SANITIZE STREQUAL "ON") OR
    ((Seastar_SANITIZE STREQUAL "DEFAULT") AND
     (CMAKE_BUILD_TYPE IN_LIST Seastar_SANITIZE_MODES)))
  if (NOT Sanitizers_FOUND)
    message (FATAL_ERROR "Sanitizers not found!")
  endif ()
  set (Seastar_Sanitizers_OPTIONS ${Sanitizers_COMPILER_OPTIONS})
  target_link_libraries (seastar
    PUBLIC
      Sanitizers::address
      Sanitizers::undefined_behavior)
endif ()

# We only need valgrind to find uninitialized memory uses, so disable
# the leak sanitizer.
# To test with valgrind run "ctest -T memcheck"
set( MEMORYCHECK_COMMAND_OPTIONS "--error-exitcode=1 --leak-check=no --trace-children=yes" )
include (CTest)

#
# We want asserts enabled on all modes, but cmake defaults to passing
# -DNDEBUG in some modes. We add -UNDEBUG to our private options to
# reenable it. To force asserts off pass -DNDEBUG in
# Seastar_CXX_FLAGS.
#
# To disable -Werror, pass -Wno-error to Seastar_CXX_FLAGS.
#
# We disable _FORTIFY_SOURCE because it generates false positives with longjmp() (src/core/thread.cc)
#

target_compile_options (seastar
  PUBLIC
    -U_FORTIFY_SOURCE)

target_compile_definitions(seastar
  PUBLIC
  SEASTAR_API_LEVEL=${Seastar_API_LEVEL}
  $<$<BOOL:${BUILD_SHARED_LIBS}>:SEASTAR_BUILD_SHARED_LIBS>)

target_compile_features(seastar
  PUBLIC
    cxx_std_${CMAKE_CXX_STANDARD})

function (seastar_supports_flag flag var)
  set (CMAKE_REQUIRED_FLAGS "${flag}")
  check_cxx_source_compiles ("int main() { return 0; }" tmp_${var})
  set (${var} ${tmp_${var}} PARENT_SCOPE)
endfunction ()

seastar_supports_flag ("-Wno-maybe-uninitialized -Werror" MaybeUninitialized_FOUND)
if (MaybeUninitialized_FOUND)
  target_compile_options (seastar
    PUBLIC
      # With std::experimental::optional it is easy to hit
      # https://gcc.gnu.org/bugzilla/show_bug.cgi?id=88897.  We disable
      # -Wmaybe-uninitialized in here since otherwise we would have to
      # disable it on many types used inside optional<>.
      -Wno-maybe-uninitialized)
endif ()

if (Seastar_SSTRING)
  target_compile_options (seastar PUBLIC -DSEASTAR_SSTRING)
endif ()

if (LinuxMembarrier_FOUND)
  list (APPEND Seastar_PRIVATE_COMPILE_DEFINITIONS SEASTAR_HAS_MEMBARRIER)

  target_link_libraries (seastar
    PRIVATE LinuxMembarrier::membarrier)
endif ()

set (Seastar_ALLOC_FAILURE_INJECTION_MODES "Dev")
if ((Seastar_ALLOC_FAILURE_INJECTION IN_LIST True_STRING_VALUES) OR
    ((Seastar_ALLOC_FAILURE_INJECTION STREQUAL "DEFAULT") AND
     (CMAKE_BUILD_TYPE IN_LIST Seastar_ALLOC_FAILURE_INJECTION_MODES)))
  target_compile_definitions (seastar
    PUBLIC SEASTAR_ENABLE_ALLOC_FAILURE_INJECTION)
endif ()

if (Seastar_TASK_BACKTRACE)
  target_compile_definitions (seastar
    PUBLIC SEASTAR_TASK_BACKTRACE)
endif ()

if (Seastar_DEBUG_ALLOCATIONS)
  target_compile_definitions (seastar
    PRIVATE SEASTAR_DEBUG_ALLOCATIONS)
endif ()

if (Sanitizers_FIBER_SUPPORT)
  list (APPEND Seastar_PRIVATE_COMPILE_DEFINITIONS SEASTAR_HAVE_ASAN_FIBER_SUPPORT)
endif ()

if (Seastar_ALLOC_PAGE_SIZE)
  target_compile_definitions (seastar
    PUBLIC SEASTAR_OVERRIDE_ALLOCATOR_PAGE_SIZE=${Seastar_ALLOC_PAGE_SIZE})
endif ()

target_compile_definitions (seastar
  PUBLIC SEASTAR_SCHEDULING_GROUPS_COUNT=${Seastar_SCHEDULING_GROUPS_COUNT})

if (Seastar_CXX_FLAGS)
  list (APPEND Seastar_PRIVATE_CXX_FLAGS ${Seastar_CXX_FLAGS})
endif ()

# When using split dwarf --gdb-index is effectively required since
# otherwise gdb is just too slow. We also want to use split dwarf in
# as many compilation units as possible.  So while these flags don't
# have to be public, we don't expect anyone to want to build seastar
# with them and some client code without.
if (Seastar_SPLIT_DWARF AND (NOT (CMAKE_BUILD_TYPE STREQUAL "Dev")))
  set (Seastar_SPLIT_DWARF_FLAG "-Wl,--gdb-index")
  target_link_libraries (seastar PUBLIC ${Seastar_SPLIT_DWARF_FLAG})
  target_compile_options (seastar PUBLIC "-gsplit-dwarf")
endif ()

if (Seastar_HEAP_PROFILING)
    set_property (
      SOURCE "src/core/memory.cc"
      PROPERTY
      COMPILE_DEFINITIONS SEASTAR_HEAPPROF)
    set_property (
      SOURCE "src/core/reactor.cc"
      PROPERTY
      COMPILE_DEFINITIONS SEASTAR_HEAPPROF)
endif ()

if (Seastar_DEFERRED_ACTION_REQUIRE_NOEXCEPT)
  list (APPEND Seastar_PRIVATE_COMPILE_DEFINITIONS SEASTAR_DEFERRED_ACTION_REQUIRE_NOEXCEPT)
endif ()

if (Seastar_DPDK)
  #
  # The DPDK architecture needs to be transitively applied to consumers of Seastar as well.
  #

  set (Seastar_ARCH_FOR_native "native")
  set (Seastar_ARCH_FOR_nhm "nehalem")
  set (Seastar_ARCH_FOR_wsm "westmere")
  set (Seastar_ARCH_FOR_snb "sandybridge")
  set (Seastar_ARCH_FOR_ivb "ivybridge")
  set (Seastar_ARCH_FOR_armv8a "armv8-a")
  set (Seastar_ARCH ${Seastar_ARCH_FOR_${Seastar_DPDK_MACHINE}})

  if (NOT Seastar_ARCH)
    message (FATAL_ERROR "Unrecognized DPDK machine identifier: ${Seastar_DPDK_MACHINE}")
  endif ()

  target_compile_options (seastar
    PUBLIC -march=${Seastar_ARCH})

  target_compile_definitions (seastar
    PUBLIC SEASTAR_HAVE_DPDK)

  # No pmd driver code will be pulled in without "--whole-archive". To
  # avoid exposing that to seastar users, combine dpdk into a single
  # .o file.
  add_custom_command (
    OUTPUT seastar-dpdk.o
    COMMAND ld -r -o seastar-dpdk.o --whole-archive ${dpdk_LIBRARIES} --no-whole-archive
  )

  # This just provides the include path to cmake
  target_link_libraries (seastar
    PUBLIC dpdk::dpdk)
endif ()

if (Seastar_HWLOC)
  list (APPEND Seastar_PRIVATE_COMPILE_DEFINITIONS SEASTAR_HAVE_HWLOC)

  target_link_libraries (seastar
    PRIVATE hwloc::hwloc)
endif ()

set_option_if_package_is_found (Seastar_IO_URING LibUring)
if (Seastar_IO_URING)
  list (APPEND Seastar_PRIVATE_COMPILE_DEFINITIONS SEASTAR_HAVE_URING)
  target_link_libraries (seastar
    PRIVATE URING::uring)
endif ()

if (Seastar_LD_FLAGS)
  target_link_options (seastar
    PRIVATE ${Seastar_LD_FLAGS})
endif ()

if (Seastar_NUMA)
  list (APPEND Seastar_PRIVATE_COMPILE_DEFINITIONS SEASTAR_HAVE_NUMA)

  target_link_libraries (seastar
    PRIVATE numactl::numactl)
endif ()

if (lz4_HAVE_COMPRESS_DEFAULT)
  list (APPEND Seastar_PRIVATE_COMPILE_DEFINITIONS SEASTAR_HAVE_LZ4_COMPRESS_DEFAULT)
endif ()

seastar_supports_flag ("-Werror=unused-result" ErrorUnused_FOUND)
if (ErrorUnused_FOUND)
  if (Seastar_UNUSED_RESULT_ERROR)
    target_compile_options (seastar
      PUBLIC -Werror=unused-result)
  else()
    target_compile_options (seastar
      PUBLIC -Wno-error=unused-result)
  endif ()
endif ()

seastar_supports_flag ("-Wno-error=#warnings" ErrorWarnings_FOUND)
if (ErrorWarnings_FOUND)
  target_compile_options (seastar
      PUBLIC "-Wno-error=#warnings")
endif ()

if ((CMAKE_BUILD_TYPE STREQUAL "Debug") OR (CMAKE_BUILD_TYPE STREQUAL "Sanitize"))
  target_compile_definitions (seastar
    PUBLIC
      SEASTAR_DEBUG
      SEASTAR_DEFAULT_ALLOCATOR
      SEASTAR_SHUFFLE_TASK_QUEUE)
endif ()

set (Seastar_DEBUG_SHARED_PTR_MODES "Debug" "Sanitize")
if (Seastar_DEBUG_SHARED_PTR IN_LIST True_STRING_VALUES OR
    ((Seastar_DEBUG_SHARED_PTR IN_LIST Default_STRING_VALUES) AND
     (CMAKE_BUILD_TYPE IN_LIST Seastar_DEBUG_SHARED_PTR_MODES)))
  target_compile_definitions (seastar
    PUBLIC
      SEASTAR_DEBUG_SHARED_PTR)
endif ()

set (Seastar_STACK_GUARD_MODES "Debug" "Sanitize" "Dev")
if ((Seastar_STACK_GUARDS STREQUAL "ON") OR
    ((Seastar_STACK_GUARDS STREQUAL "DEFAULT") AND
     (CMAKE_BUILD_TYPE IN_LIST Seastar_STACK_GUARD_MODES)))
  # check for -fstack-clash-protection together with -Werror, because
  # otherwise clang can soft-fail (return 0 but emit a warning) instead.
  seastar_supports_flag ("-fstack-clash-protection -Werror" StackClashProtection_FOUND)
  if (StackClashProtection_FOUND)
    target_compile_options (seastar
      PUBLIC
      -fstack-clash-protection)
  endif ()
  target_compile_definitions (seastar
    PRIVATE
      SEASTAR_THREAD_STACK_GUARDS)
endif ()

if ((CMAKE_BUILD_TYPE STREQUAL "Dev") OR (CMAKE_BUILD_TYPE STREQUAL "Debug"))
  target_compile_definitions (seastar
    PUBLIC
      SEASTAR_TYPE_ERASE_MORE)
endif ()

target_compile_definitions (seastar
  PRIVATE ${Seastar_PRIVATE_COMPILE_DEFINITIONS})

target_compile_options (seastar
  PRIVATE ${Seastar_PRIVATE_CXX_FLAGS})

set_target_properties (seastar
  PROPERTIES
    CXX_STANDARD ${CMAKE_CXX_STANDARD}
    CXX_EXTENSIONS ON)

add_library (seastar_private INTERFACE)

target_compile_definitions (seastar_private
  INTERFACE ${Seastar_PRIVATE_COMPILE_DEFINITIONS})

target_compile_options (seastar_private
  INTERFACE ${Seastar_PRIVATE_CXX_FLAGS})

target_link_libraries (seastar_private
  INTERFACE seastar)

#
# The testing library.
#

if (Seastar_INSTALL OR Seastar_TESTING)
  add_library (seastar_testing
    include/seastar/testing/entry_point.hh
    include/seastar/testing/exchanger.hh
    include/seastar/testing/random.hh
    include/seastar/testing/seastar_test.hh
    include/seastar/testing/test_case.hh
    include/seastar/testing/test_runner.hh
    include/seastar/testing/thread_test_case.hh
    src/testing/entry_point.cc
    src/testing/random.cc
    src/testing/seastar_test.cc
    src/testing/test_runner.cc)

  add_library (Seastar::seastar_testing ALIAS seastar_testing)

  target_compile_definitions (seastar_testing
    PRIVATE ${Seastar_PRIVATE_COMPILE_DEFINITIONS})

  target_compile_options (seastar_testing
    PRIVATE ${Seastar_PRIVATE_CXX_FLAGS})

  target_link_libraries (seastar_testing
    PUBLIC
      Boost::unit_test_framework
      Boost::dynamic_linking
      seastar)

  add_library(seastar_perf_testing
    src/testing/random.cc
    include/seastar/testing/perf_tests.hh
    tests/perf/perf_tests.cc
    tests/perf/linux_perf_event.cc)
  add_library (Seastar::seastar_perf_testing ALIAS seastar_perf_testing)
  target_compile_definitions (seastar_perf_testing
    PRIVATE ${Seastar_PRIVATE_COMPILE_DEFINITIONS})
  target_compile_options (seastar_perf_testing
    PRIVATE ${Seastar_PRIVATE_CXX_FLAGS})
  target_link_libraries (seastar_perf_testing
    PUBLIC
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

if (CMAKE_BUILD_TYPE STREQUAL "Dev")
  include (CheckHeaders)
  add_custom_target (checkheaders)
  foreach (lib seastar seastar_testing seastar_perf_testing)
    if (TARGET ${lib})
      seastar_check_self_contained (${lib}
        INCLUDE "\\.hh$"
        # impl.hh headers are internal implementations of .hh, so they are not
        # compilable. let's exclude them from the files to be checked.
        EXCLUDE "_impl.hh$|-impl.hh$")
    endif ()
  endforeach ()
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

  # Set paths in pkg-config files for installation.
  set (Seastar_PKG_CONFIG_PREFIX ${CMAKE_INSTALL_PREFIX})
  set (Seastar_PKG_CONFIG_LIBDIR ${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR})
  set (Seastar_PKG_CONFIG_SEASTAR_INCLUDE_FLAGS "-I${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_INCLUDEDIR}")

  configure_file (
    ${CMAKE_CURRENT_SOURCE_DIR}/pkgconfig/seastar.pc.in
    ${CMAKE_CURRENT_BINARY_DIR}/pkgconfig/seastar-install.pc.in
    @ONLY)

  configure_file (
    ${CMAKE_CURRENT_SOURCE_DIR}/pkgconfig/seastar-testing.pc.in
    ${CMAKE_CURRENT_BINARY_DIR}/pkgconfig/seastar-testing-install.pc.in
    @ONLY)

  # Set paths in pkg-config files for direct use in the build directory.
  set (Seastar_PKG_CONFIG_PREFIX ${CMAKE_CURRENT_BINARY_DIR})
  set (Seastar_PKG_CONFIG_LIBDIR ${CMAKE_CURRENT_BINARY_DIR})
  set (Seastar_PKG_CONFIG_SEASTAR_INCLUDE_FLAGS "-I${CMAKE_CURRENT_SOURCE_DIR}/include -I${CMAKE_CURRENT_BINARY_DIR}/gen/include")

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

  file (GENERATE
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/seastar-install.pc
    INPUT ${CMAKE_CURRENT_BINARY_DIR}/pkgconfig/seastar-install.pc.in)

  file (GENERATE
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/seastar-testing-install.pc
    INPUT ${CMAKE_CURRENT_BINARY_DIR}/pkgconfig/seastar-testing-install.pc.in)

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
      seastar_perf_testing
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
      ${CMAKE_CURRENT_SOURCE_DIR}/cmake/FindGnuTLS.cmake
      ${CMAKE_CURRENT_SOURCE_DIR}/cmake/FindLinuxMembarrier.cmake
      ${CMAKE_CURRENT_SOURCE_DIR}/cmake/FindSanitizers.cmake
      ${CMAKE_CURRENT_SOURCE_DIR}/cmake/FindSourceLocation.cmake
      ${CMAKE_CURRENT_SOURCE_DIR}/cmake/FindStdAtomic.cmake
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
      ${CMAKE_CURRENT_SOURCE_DIR}/cmake/SeastarDependencies.cmake
      ${CMAKE_CURRENT_SOURCE_DIR}/cmake/FindLibUring.cmake
    DESTINATION ${install_cmakedir})

  install (
    DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/cmake/code_tests
    DESTINATION ${install_cmakedir})

  install (
    FILES ${CMAKE_CURRENT_BINARY_DIR}/seastar-install.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
    RENAME seastar.pc)

  install (
    FILES ${CMAKE_CURRENT_BINARY_DIR}/seastar-testing-install.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
    RENAME seastar-testing.pc)

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
