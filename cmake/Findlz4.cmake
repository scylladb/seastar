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

find_package (PkgConfig)

pkg_search_module (PC_lz4
  REQUIRED
  QUIET
  liblz4)

find_path (lz4_INCLUDE_DIR
  NAMES lz4.h
  PATHS ${PC_lz4_INCLUDE_DIRS})

find_library (lz4_LIBRARY
  NAMES lz4
  PATHS ${PC_lz4_LIBRARY_DIRS})

set (lz4_VERSION ${PC_lz4_VERSION})

include (FindPackageHandleStandardArgs)

find_package_handle_standard_args (lz4
  FOUND_VAR lz4_FOUND
  REQUIRED_VARS
    lz4_INCLUDE_DIR
    lz4_LIBRARY
  VERSION_VAR lz4_VERSION)

if (lz4_FOUND)
  set (CMAKE_REQUIRED_LIBRARIES ${lz4_LIBRARY})
  include (CheckSymbolExists)

  check_symbol_exists (LZ4_compress_default
    ${lz4_INCLUDE_DIR}/lz4.h
    lz4_HAVE_COMPRESS_DEFAULT)

  set (lz4_INCLUDE_DIRS ${lz4_INCLUDE_DIR})
endif ()

if (lz4_FOUND AND NOT (TARGET lz4::lz4))
  add_library (lz4::lz4 UNKNOWN IMPORTED)

  set_target_properties (lz4::lz4
    PROPERTIES
      IMPORTED_LOCATION ${lz4_LIBRARY}
      INTERFACE_COMPILE_OPTIONS "${PC_lz4_CFLAGS_OTHER}"
      INTERFACE_INCLUDE_DIRECTORIES ${lz4_INCLUDE_DIR})
endif ()

mark_as_advanced (
  lz4_INCLUDE_DIR
  lz4_LIBRARY)
