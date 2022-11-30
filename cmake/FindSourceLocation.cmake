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
# Copyright (C) 2022 Kefu Chai ( tchaikov@gmail.com )
#

include (CheckCXXSourceCompiles)
include (CheckCXXSourceRuns)
include (CMakePushCheckState)

cmake_push_check_state ()
file (READ ${CMAKE_CURRENT_LIST_DIR}/code_tests/Source_location_test.cc _source_location_test_code)
set(CMAKE_REQUIRED_FLAGS "${CMAKE_CXX${CMAKE_CXX_STANDARD}_STANDARD_COMPILE_OPTION}")
check_cxx_source_compiles ("${_source_location_test_code}" CxxSourceLocation_SUPPORTED)

file (READ ${CMAKE_CURRENT_LIST_DIR}/code_tests/Source_location_default_argument.cc _source_location_test_code)
set(CMAKE_REQUIRED_FLAGS "${CMAKE_CXX${CMAKE_CXX_STANDARD}_STANDARD_COMPILE_OPTION}")
# see also https://cplusplus.github.io/CWG/issues/2631.html
check_cxx_source_runs ("${_source_location_test_code}" CxxSourceLocation_IMPLEMENTS_CWG2631)
cmake_pop_check_state ()

if (NOT (TARGET SourceLocation::source_location))
  add_library (SourceLocation::source_location INTERFACE IMPORTED)
  if  ((NOT CxxSourceLocation_SUPPORTED) OR
       (NOT CxxSourceLocation_IMPLEMENTS_CWG2631))
    set_target_properties (SourceLocation::source_location
      PROPERTIES
        INTERFACE_COMPILE_DEFINITIONS SEASTAR_BROKEN_SOURCE_LOCATION)
  endif ()
endif ()
