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

# Logical target for all demos.
add_custom_target (demos)

macro (seastar_add_demo name)
  set (args ${ARGN})

  cmake_parse_arguments (
    parsed_args
    ""
    ""
    "SOURCES"
    ${args})

  set (target demo_${name})
  add_executable (${target} ${parsed_args_SOURCES})

  target_include_directories (${target}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

  target_link_libraries (${target}
    PRIVATE
      Boost::program_options
      seastar_private)

  set_target_properties (${target}
    PROPERTIES
      OUTPUT_NAME ${name}_demo)

  add_dependencies (demos ${target})
endmacro ()

seastar_add_demo (block_discard
  SOURCES block_discard_demo.cc)

if (${Seastar_API_LEVEL} GREATER_EQUAL 3)
  seastar_add_demo (coroutines
    SOURCES coroutines_demo.cc)
endif ()

seastar_add_demo (hello-world
  SOURCES hello-world.cc)

seastar_add_demo (websocket_server
  SOURCES websocket_server_demo.cc)

seastar_add_demo (echo
  SOURCES echo_demo.cc)

seastar_add_demo (ip
  SOURCES ip_demo.cc)

seastar_add_demo (line_count
  SOURCES line_count_demo.cc)

seastar_add_demo (l3
  SOURCES l3_demo.cc)

seastar_add_demo (rpc
  SOURCES rpc_demo.cc)

seastar_add_demo (scheduling_group
  SOURCES scheduling_group_demo.cc)

seastar_add_demo (tcp
  SOURCES tcp_demo.cc)

seastar_add_demo (tcp_sctp_client
  SOURCES tcp_sctp_client_demo.cc)

seastar_add_demo (tcp_sctp_server
  SOURCES tcp_sctp_server_demo.cc)

seastar_add_demo (tls_echo_server
  SOURCES
    tls_echo_server.hh
    tls_echo_server_demo.cc)

seastar_add_demo (tls_simple_client
  SOURCES
    tls_echo_server.hh
    tls_simple_client_demo.cc)

seastar_add_demo (udp_client
  SOURCES udp_client_demo.cc)

seastar_add_demo (udp_server
  SOURCES udp_server_demo.cc)

seastar_add_demo (udp_zero_copy
  SOURCES udp_zero_copy_demo.cc)

seastar_add_demo (sharded_parameter
  SOURCES sharded_parameter_demo.cc)

seastar_add_demo (file
  SOURCES file_demo.cc)

seastar_add_demo (tutorial_examples
  SOURCES tutorial_examples.cc)

seastar_add_demo (http_client
  SOURCES http_client_demo.cc)

if (Seastar_MODULE)
  add_executable (hello_cxx_module)
  target_sources (hello_cxx_module
    PRIVATE
      hello-cxx-module.cc)
  target_link_libraries (hello_cxx_module
    PRIVATE
      seastar-module)
endif ()
