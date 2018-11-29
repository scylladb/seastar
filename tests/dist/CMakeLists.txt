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

add_custom_command (
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/consumer
  DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt
    ${CMAKE_CURRENT_SOURCE_DIR}/consumer/CMakeLists.txt
    ${CMAKE_CURRENT_SOURCE_DIR}/consumer/Makefile
    ${CMAKE_CURRENT_SOURCE_DIR}/consumer/cmake_consumer.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/consumer/pkgconfig_consumer.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/consumer/recipe/test_dist.cmake
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/consumer ${CMAKE_CURRENT_BINARY_DIR}/consumer)

configure_file (
  ${Seastar_SOURCE_DIR}/cooking.sh
  ${CMAKE_CURRENT_BINARY_DIR}/consumer/cooking.sh
  COPYONLY)

add_custom_target (test_dist_consumer_test_run
  DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/consumer
  COMMAND
    ${CMAKE_COMMAND} -E env
    CONSUMER_SOURCE_DIR=${CMAKE_CURRENT_BINARY_DIR}/consumer
    SEASTAR_SOURCE_DIR=${Seastar_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/consumer_test.sh
  USES_TERMINAL)

add_test (
  NAME Seastar.dist.consumer
  COMMAND ${CMAKE_COMMAND} --build ${Seastar_BINARY_DIR} --target test_dist_consumer_test_run)

add_custom_target (test_dist
  COMMAND ctest --verbose -R Seastar.dist
  USES_TERMINAL)
