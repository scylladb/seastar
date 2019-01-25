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

set (Seastar_APP_MEMCACHED_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set (Seastar_APP_MEMCACHED_BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR})

seastar_generate_ragel (
  TARGET app_memcached_ascii
  VAR app_memcached_ascii_file
  IN_FILE ${CMAKE_CURRENT_SOURCE_DIR}/ascii.rl
  OUT_FILE ${CMAKE_CURRENT_BINARY_DIR}/ascii.hh)

seastar_add_app (memcached
  SOURCES
    ${app_memcached_ascii_file}
    memcache.cc
    memcached.hh)

target_include_directories (app_memcached
  PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

add_dependencies (app_memcached app_memcached_ascii)

#
# Tests.
#

if (Seastar_TESTING)
  add_subdirectory (tests)
endif ()
