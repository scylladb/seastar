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

cmake_policy (PUSH)
cmake_policy (SET CMP0057 NEW)

if(NOT Sanitizers_FIND_COMPONENTS)
  set(Sanitizers_FIND_COMPONENTS
    address
    undefined_behavior)
endif()

foreach (component ${Sanitizers_FIND_COMPONENTS})
  string (TOUPPER ${component} COMPONENT)
  set (compile_options "Sanitizers_${COMPONENT}_COMPILE_OPTIONS")
  if (component STREQUAL "address")
    list (APPEND ${compile_options} -fsanitize=address)
  elseif (component STREQUAL "undefined_behavior")
    list (APPEND ${compile_options} -fsanitize=undefined)
  else ()
    message (FATAL_ERROR "Unsupported sanitizer: ${component}")
  endif ()
  list(APPEND Sanitizers_COMPILE_OPTIONS "${${compile_options}}")
  unset (compile_options)
endforeach ()

include(CheckCXXSourceCompiles)
include(CMakePushCheckState)

# -fsanitize=address cannot be combined with -fsanitize=thread, so let's test
# the combination of the compiler options.
cmake_push_check_state()
string (REPLACE ";" " " CMAKE_REQUIRED_FLAGS "${Sanitizers_COMPILE_OPTIONS}")
set(CMAKE_REQUIRED_FLAGS ${Sanitizers_COMPILE_OPTIONS})
check_cxx_source_compiles("int main() {}"
  Sanitizers_SUPPORTED)
if (Sanitizers_SUPPORTED)
  if ("address" IN_LIST Sanitizers_FIND_COMPONENTS)
    file (READ ${CMAKE_CURRENT_LIST_DIR}/code_tests/Sanitizers_fiber_test.cc _sanitizers_fiber_test_code)
    check_cxx_source_compiles ("${_sanitizers_fiber_test_code}"
      Sanitizers_FIBER_SUPPORT)
  endif ()
endif ()
cmake_pop_check_state()

include (FindPackageHandleStandardArgs)

find_package_handle_standard_args (Sanitizers
  REQUIRED_VARS
    Sanitizers_COMPILE_OPTIONS
    Sanitizers_SUPPORTED)

if (Sanitizers_FOUND)
  foreach (component ${Sanitizers_FIND_COMPONENTS})
    string (TOUPPER ${component} COMPONENT)
    set (library Sanitizers::${component})
    if (NOT TARGET ${library})
      add_library (${library} INTERFACE IMPORTED)
      set_target_properties (${library}
        PROPERTIES
          INTERFACE_COMPILE_OPTIONS "${Sanitizers_${COMPONENT}_COMPILE_OPTIONS}"
          INTERFACE_LINK_LIBRARIES "${Sanitizers_${COMPONENT}_COMPILE_OPTIONS}")
    endif ()
  endforeach ()
endif ()

cmake_policy (POP)
