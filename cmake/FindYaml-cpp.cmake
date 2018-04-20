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

pkg_check_modules (PC_Yaml-cpp QUIET yaml-cpp)

find_path (Yaml-cpp_INCLUDE_DIR
  NAMES yaml-cpp/yaml.h
  PATHS ${PC_Yaml-cpp_INCLUDE_DIRS})

find_library (Yaml-cpp_LIBRARY
  NAMES yaml-cpp
  PATHS ${PC_Yaml-cpp_LIBRARY_DIRS})

set (Yaml-cpp_VERSION ${PC_Yaml-cpp_VERSION})

include (FindPackageHandleStandardArgs)

find_package_handle_standard_args (Yaml-cpp
  FOUND_VAR Yaml-cpp_FOUND
  REQUIRED_VARS
    Yaml-cpp_LIBRARY
    Yaml-cpp_INCLUDE_DIR
  VERSION_VAR Yaml-cpp_VERSION)

if (Yaml-cpp_FOUND)
  set (Yaml-cpp_LIBRARIES ${Yaml-cpp_LIBRARY})
  set (Yaml-cpp_INCLUDE_DIRS ${Yaml-cpp_INCLUDE_DIR})
  set (Yaml-cpp_DEFINITIONS ${PC_Yaml-cpp_CFLAGS_OTHER})
endif ()

if (Yaml-cpp_FOUND AND NOT TARGET Yaml-cpp::yaml-cpp)
  add_library (Yaml-cpp::yaml-cpp UNKNOWN IMPORTED)

  set_target_properties (Yaml-cpp::yaml-cpp PROPERTIES
    IMPORTED_LOCATION "${Yaml-cpp_LIBRARY}"
    INTERFACE_COMPILE_OPTIONS "${PC_Yaml-cpp_CFLAGS_OTHER}"
    INTERFACE_INCLUDE_DIRECTORIES "${Yaml-cpp_INCLUDE_DIR}")
endif ()
