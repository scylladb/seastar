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

# Logical target for all applications.
add_custom_target (apps)

macro (seastar_add_app name)
  set (args ${ARGN})

  cmake_parse_arguments (
    parsed_args
    ""
    ""
    "SOURCES"
    ${args})

  set (target app_${name})
  add_executable (${target} ${parsed_args_SOURCES})

  target_include_directories (${target}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

  target_link_libraries (${target}
    PRIVATE seastar_private)

  set_target_properties (${target}
    PROPERTIES
      OUTPUT_NAME ${name})

  add_dependencies (apps ${target})
endmacro ()

add_subdirectory (httpd)
add_subdirectory (io_tester)
add_subdirectory (rpc_tester)
add_subdirectory (iotune)
add_subdirectory (memcached)
add_subdirectory (seawreck)
