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

pkg_search_module (hwloc_PC
  QUIET
  hwloc)

find_library (hwloc_LIBRARY
  NAMES hwloc
  HINTS
    ${hwloc_PC_LIBDIR}
    ${hwloc_PC_LIBRARY_DIRS})

find_path (hwloc_INCLUDE_DIR
  NAMES hwloc.h
  HINTS
    ${hwloc_PC_INCLUDEDIR}
    ${hwloc_PC_INCLUDEDIRS})

mark_as_advanced (
  hwloc_LIBRARY
  hwloc_INCLUDE_DIR)

include (FindPackageHandleStandardArgs)

find_package_handle_standard_args (hwloc
  REQUIRED_VARS
    hwloc_LIBRARY
    hwloc_INCLUDE_DIR
  VERSION_VAR hwloc_PC_VERSION)

set (hwloc_LIBRARIES ${hwloc_LIBRARY})
set (hwloc_INCLUDE_DIRS ${hwloc_INCLUDE_DIR})

if (hwloc_FOUND AND NOT (TARGET hwloc::hwloc))
  add_library (hwloc::hwloc UNKNOWN IMPORTED)

  set_target_properties (hwloc::hwloc
    PROPERTIES
      IMPORTED_LOCATION ${hwloc_LIBRARY}
      INTERFACE_INCLUDE_DIRECTORIES ${hwloc_INCLUDE_DIRS})
endif ()
