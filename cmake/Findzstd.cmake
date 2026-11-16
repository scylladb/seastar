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
# Copyright (C) 2026 Scylladb, Ltd.
#

find_package (PkgConfig REQUIRED)

pkg_search_module (PC_zstd QUIET libzstd)

find_library (zstd_LIBRARY
  NAMES zstd
  HINTS
    ${PC_zstd_LIBDIR}
    ${PC_zstd_LIBRARY_DIRS})

find_path (zstd_INCLUDE_DIR
  NAMES zstd.h
  HINTS
    ${PC_zstd_INCLUDEDIR}
    ${PC_zstd_INCLUDE_DIRS})

mark_as_advanced (
  zstd_LIBRARY
  zstd_INCLUDE_DIR)

# The version is that of the header that was found, which pkg-config may know
# nothing about.
if (zstd_INCLUDE_DIR AND EXISTS ${zstd_INCLUDE_DIR}/zstd.h)
  foreach (part MAJOR MINOR RELEASE)
    file (STRINGS ${zstd_INCLUDE_DIR}/zstd.h zstd_version_line
      REGEX "^#define[ \t]+ZSTD_VERSION_${part}[ \t]+[0-9]+")
    string (REGEX REPLACE ".*ZSTD_VERSION_${part}[ \t]+([0-9]+).*" "\\1"
      zstd_version_${part} "${zstd_version_line}")
  endforeach ()
  set (zstd_VERSION
    ${zstd_version_MAJOR}.${zstd_version_MINOR}.${zstd_version_RELEASE})
endif ()

include (FindPackageHandleStandardArgs)

find_package_handle_standard_args (zstd
  REQUIRED_VARS
    zstd_LIBRARY
    zstd_INCLUDE_DIR
  VERSION_VAR zstd_VERSION)

if (zstd_FOUND)
  set (zstd_LIBRARIES ${zstd_LIBRARY})
  set (zstd_INCLUDE_DIRS ${zstd_INCLUDE_DIR})

  if (NOT (TARGET zstd::zstd))
    add_library (zstd::zstd UNKNOWN IMPORTED)

    set_target_properties (zstd::zstd
      PROPERTIES
        IMPORTED_LOCATION ${zstd_LIBRARY}
        INTERFACE_INCLUDE_DIRECTORIES ${zstd_INCLUDE_DIRS})
  endif ()
endif ()
