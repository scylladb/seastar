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

pkg_search_module (PC_hwloc
  REQUIRED
  QUIET
  hwloc)

find_path (hwloc_INCLUDE_DIR
  NAMES hwloc.h
  PATHS ${PC_hwloc_INCLUDE_DIRS})

find_library (hwloc_LIBRARY
  NAMES hwloc
  PATHS ${PC_hwloc_LIBRARY_DIRS})

set (hwloc_VERSION ${PC_hwloc_VERSION})

include (FindPackageHandleStandardArgs)

find_package_handle_standard_args (hwloc
  FOUND_VAR hwloc_FOUND
  REQUIRED_VARS
    hwloc_INCLUDE_DIR
    hwloc_LIBRARY
  VERSION_VAR hwloc_VERSION)

if (hwloc_FOUND)
  set (hwloc_INCLUDE_DIRS ${hwloc_INCLUDE_DIR})
endif ()

if (hwloc_FOUND AND NOT (TARGET hwloc::hwloc))
  add_library (hwloc::hwloc UNKNOWN IMPORTED)

  set_target_properties (hwloc::hwloc
    PROPERTIES
      IMPORTED_LOCATION ${hwloc_LIBRARY}
      INTERFACE_COMPILE_OPTIONS "${PC_hwloc_CFLAGS_OTHER}"
      INTERFACE_INCLUDE_DIRECTORIES ${hwloc_INCLUDE_DIR})
endif ()

mark_as_advanced (
  hwloc_INCLUDE_DIR
  hwloc_LIBRARY)
