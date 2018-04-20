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

pkg_check_modules (PC_Lz4 QUIET liblz4)

find_path (Lz4_INCLUDE_DIR
  NAMES lz4.h
  PATHS ${PC_Lz4_INCLUDE_DIRS})

find_library (Lz4_LIBRARY
  NAMES lz4
  PATHS ${PC_Lz4_LIBRARY_DIRS})

set (Lz4_VERSION ${PC_Lz4_VERSION})

include (FindPackageHandleStandardArgs)

find_package_handle_standard_args (Lz4
  FOUND_VAR Lz4_FOUND
  REQUIRED_VARS
    Lz4_LIBRARY
    Lz4_INCLUDE_DIR
  VERSION_VAR Lz4_VERSION)

if (Lz4_FOUND)
  set (Lz4_LIBRARIES ${Lz4_LIBRARY})
  set (Lz4_INCLUDE_DIRS ${Lz4_INCLUDE_DIR})
  set (Lz4_DEFINITIONS ${PC_Lz4_CFLAGS_OTHER})
endif ()

if (Lz4_FOUND AND NOT TARGET Lz4::lz4)
  add_library (Lz4::lz4 UNKNOWN IMPORTED)

  set_target_properties (Lz4::lz4 PROPERTIES
    IMPORTED_LOCATION "${Lz4_LIBRARY}"
    INTERFACE_COMPILE_OPTIONS "${PC_Lz4_CFLAGS_OTHER}"
    INTERFACE_INCLUDE_DIRECTORIES "${Lz4_INCLUDE_DIR}")
endif ()
