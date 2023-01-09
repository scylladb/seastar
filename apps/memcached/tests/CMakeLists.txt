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

if (Seastar_EXECUTE_ONLY_FAST_TESTS)
  set (memcached_test_args --fast)
else ()
  set (memcached_test_args "")
endif ()

add_custom_target (app_memcached_test_memcached_run
  DEPENDS
    ${memcached_app}
    ${CMAKE_CURRENT_SOURCE_DIR}/test.py
    ${CMAKE_CURRENT_SOURCE_DIR}/test_memcached.py
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test.py --memcached $<TARGET_FILE:app_memcached> ${memcached_test_args}
  USES_TERMINAL)

add_test (
  NAME Seastar.app.memcached.memcached
  COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target app_memcached_test_memcached_run)

set_tests_properties (Seastar.app.memcached.memcached
  PROPERTIES
    TIMEOUT ${Seastar_TEST_TIMEOUT}
    ENVIRONMENT ${Seastar_TEST_ENVIRONMENT})

add_executable (app_memcached_test_ascii
  test_ascii_parser.cc)

add_dependencies (app_memcached_test_ascii app_memcached)

target_include_directories (app_memcached_test_ascii
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${Seastar_APP_MEMCACHED_BINARY_DIR}
    ${Seastar_APP_MEMCACHED_SOURCE_DIR})

target_compile_definitions (app_memcached_test_ascii
  PRIVATE SEASTAR_TESTING_MAIN)

target_link_libraries (app_memcached_test_ascii
  PRIVATE
    seastar_private
    seastar_testing)

add_custom_target (app_memcached_test_ascii_run
  DEPENDS app_memcached_test_ascii
  COMMAND app_memcached_test_ascii -- -c 2
  USES_TERMINAL)

add_test (
  NAME Seastar.app.memcached.ascii
  COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target app_memcached_test_ascii_run)

set_tests_properties (Seastar.app.memcached.ascii
  PROPERTIES
    TIMEOUT ${Seastar_TEST_TIMEOUT}
    ENVIRONMENT ${Seastar_TEST_ENVIRONMENT})
