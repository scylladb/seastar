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
# Copyright (C) 2023 Scylladb, Ltd.
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

include (FindPackageHandleStandardArgs)

find_package_handle_standard_args (zstd
  REQUIRED_VARS
    zstd_LIBRARY
    zstd_INCLUDE_DIR
  VERSION_VAR PC_zstd_VERSION)

if (zstd_FOUND)
  set (CMAKE_REQUIRED_LIBRARIES ${zstd_LIBRARY})

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
