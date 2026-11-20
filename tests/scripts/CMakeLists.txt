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
# Copyright (C) 2026 ScyllaDB
#

find_package (Python3 COMPONENTS Interpreter REQUIRED)

# The tests of the exact output run the fake tools of fake-bin/, the tests on a
# real executable run the tools installed on the system.
foreach (name stall_analyser addr2line)
  add_test (
    NAME Seastar.scripts.${name}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/${name}_test.py)
  set_tests_properties (Seastar.scripts.${name}
    PROPERTIES
      ENVIRONMENT SCRIPTS_TEST_FAKES=1
      TIMEOUT ${Seastar_TEST_TIMEOUT})
endforeach ()

add_test (
  NAME Seastar.scripts.real_tools
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/real_tools_test.py)
set_tests_properties (Seastar.scripts.real_tools
  PROPERTIES
    TIMEOUT ${Seastar_TEST_TIMEOUT})
