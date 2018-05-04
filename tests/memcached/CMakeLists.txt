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

if (${SEASTAR_EXECUTE_ONLY_FAST_TESTS})
  set (fast --fast)
else ()
  set (fast "")
endif()

# Special case dependency since `memcached_test` depends on the `memcached` target being built.
add_dependencies (tests memcached)

add_test (
  NAME memcached_test
  COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/test.py" ${fast} --memcached $<TARGET_FILE:memcached>)

add_seastar_test (NAME memcached_ascii_parser_test
  SUITE
  SOURCES test_ascii_parser.cc)

add_dependencies (memcached_ascii_parser_test memcached)
