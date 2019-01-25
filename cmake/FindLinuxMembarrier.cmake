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

find_path (LinuxMembarrier_INCLUDE_DIR
  NAMES linux/membarrier.h)

include (CheckCXXSourceCompiles)
file (READ ${CMAKE_CURRENT_LIST_DIR}/code_tests/LinuxMembarrier_test.cc _linuxmembarrier_test_code)
check_cxx_source_compiles ("${_linuxmembarrier_test_code}" LinuxMembarrier_FOUND)

if (LinuxMembarrier_FOUND)
  set (LinuxMembarrier_INCLUDE_DIRS ${LinuxMembarrier_INCLUDE_DIR})
endif ()

if (LinuxMembarrier_FOUND AND NOT (TARGET LinuxMembarrier::membarrier))
  add_library (LinuxMembarrier::membarrier INTERFACE IMPORTED)

  set_target_properties (LinuxMembarrier::membarrier
    PROPERTIES
      INTERFACE_INCLUDE_DIRECTORIES ${LinuxMembarrier_INCLUDE_DIRS})
endif ()
