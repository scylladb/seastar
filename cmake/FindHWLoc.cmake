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

pkg_check_modules (PC_HWLoc QUIET hwloc)

find_path (HWLoc_INCLUDE_DIR
  NAMES hwloc.h
  PATHS ${PC_HWLoc_INCLUDE_DIRS})

find_library (HWLoc_LIBRARY
  NAMES hwloc
  PATHS ${PC_HWLoc_LIBRARY_DIRS})

set (HWLoc_VERSION ${PC_HWLoc_VERSION})

include (FindPackageHandleStandardArgs)

find_package_handle_standard_args (HWLoc
  FOUND_VAR HWLoc_FOUND
  REQUIRED_VARS
    HWLoc_LIBRARY
    HWLoc_INCLUDE_DIR
  VERSION_VAR HWLoc_VERSION)

if (HWLoc_FOUND)
  set (HWLoc_LIBRARIES ${HWLoc_LIBRARY})
  set (HWLoc_INCLUDE_DIRS ${HWLoc_INCLUDE_DIR})
  set (HWLoc_DEFINITIONS ${PC_HWLoc_CFLAGS_OTHER})
endif ()

if (HWLoc_FOUND AND NOT TARGET HWLoc::hwloc)
  add_library (HWLoc::hwloc UNKNOWN IMPORTED)

  set_target_properties (HWLoc::hwloc PROPERTIES
    IMPORTED_LOCATION "${HWLoc_LIBRARY}"
    INTERFACE_COMPILE_OPTIONS "${PC_HWLoc_CFLAGS_OTHER}"
    INTERFACE_INCLUDE_DIRECTORIES "${HWLoc_INCLUDE_DIR}")
endif ()
