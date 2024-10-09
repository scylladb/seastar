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

find_package (PkgConfig REQUIRED)

pkg_check_modules (PC_c-ares QUIET libcares)

find_library (c-ares_LIBRARY
  NAMES cares
  HINTS
    ${PC_c-ares_LIBDIR}
    ${PC_c-ares_LIBRARY_DIRS})

find_path (c-ares_INCLUDE_DIR
  NAMES ares_dns.h
  HINTS
    ${PC_c-ares_INCLUDEDIR}
    ${PC_c-ares_INCLUDE_DIRS})

if (c-ares_INCLUDE_DIR)
  foreach (v MAJOR MINOR PATCH)
    file(STRINGS "${c-ares_INCLUDE_DIR}/ares_version.h" ares_VERSION_LINE
      REGEX "^#define[ \t]+ARES_VERSION_${v}[ \t]+[0-9]+$")
    if (ares_VERSION_LINE MATCHES "ARES_VERSION_${v} ([0-9]+)")
      set (c-ares_VERSION_${v} "${CMAKE_MATCH_1}")
    endif ()
    unset (ares_VERSION_LINE)
  endforeach ()
  set (c-ares_VERSION ${c-ares_VERSION_MAJOR}.${c-ares_VERSION_MINOR}.${c-ares_VERSION_PATCH})
endif ()

mark_as_advanced (
  c-ares_LIBRARY
  c-ares_INCLUDE_DIR)

include (FindPackageHandleStandardArgs)

find_package_handle_standard_args (c-ares
  REQUIRED_VARS
    c-ares_LIBRARY
    c-ares_INCLUDE_DIR
  VERSION_VAR c-ares_VERSION)

if (c-ares_FOUND)
  set (c-ares_LIBRARIES ${c-ares_LIBRARY})
  set (c-ares_INCLUDE_DIRS ${c-ares_INCLUDE_DIR})
  if (NOT (TARGET c-ares::cares))
    add_library (c-ares::cares UNKNOWN IMPORTED)

    set_target_properties (c-ares::cares
      PROPERTIES
        IMPORTED_LOCATION ${c-ares_LIBRARY}
        INTERFACE_INCLUDE_DIRECTORIES ${c-ares_INCLUDE_DIRS})
  endif ()
endif ()
