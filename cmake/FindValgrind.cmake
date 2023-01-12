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


find_package (PkgConfig REQUIRED)

pkg_check_modules (PC_valgrind QUIET valgrind)

find_path (Valgrind_INCLUDE_DIR
  NAMES valgrind/valgrind.h
  HINTS
    ${PC_valgrind_INCLUDEDIR}
    ${PC_valgrind_INCLUDE_DIRS})

mark_as_advanced (
  Valgrind_INCLUDE_DIR)

include (FindPackageHandleStandardArgs)

find_package_handle_standard_args (Valgrind
  REQUIRED_VARS
    Valgrind_INCLUDE_DIR)

if (Valgrind_FOUND)
  set (Valgrind_INCLUDE_DIRS ${Valgrind_INCLUDE_DIR})

  if (NOT (TARGET Valgrind::valgrind))
    add_library (Valgrind::valgrind INTERFACE IMPORTED)

    set_target_properties (Valgrind::valgrind
      PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES ${Valgrind_INCLUDE_DIRS})
  endif ()
endif ()
