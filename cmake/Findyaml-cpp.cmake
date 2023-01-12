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

pkg_search_module (PC_yaml-cpp QUIET yaml-cpp)

find_library (yaml-cpp_LIBRARY_RELEASE
 NAMES yaml-cpp
 HINTS
   ${PC_yaml-cpp_LIBDIR}
   ${PC_yaml-cpp_LIBRARY_DIRS})

find_library (yaml-cpp_LIBRARY_DEBUG
 NAMES yaml-cppd
 HINTS
   ${PC_yaml-cpp_LIBDIR}
   ${PC_yaml-cpp_LIBRARY_DIRS})

include (SelectLibraryConfigurations)
select_library_configurations (yaml-cpp)

find_path (yaml-cpp_INCLUDE_DIR
  NAMES yaml-cpp/yaml.h
  PATH_SUFFIXES yaml-cpp
  HINTS
    ${PC_yaml-cpp_INCLUDEDIR}
    ${PC_yaml-cpp_INCLUDE_DIRS})

mark_as_advanced (
  yaml-cpp_LIBRARY_RELEASE
  yaml-cpp_LIBRARY_DEBUG
  yaml-cpp_INCLUDE_DIR)

include (FindPackageHandleStandardArgs)

find_package_handle_standard_args (yaml-cpp
  REQUIRED_VARS
    yaml-cpp_LIBRARY
    yaml-cpp_INCLUDE_DIR
  VERSION_VAR yaml-cpp_VERSION)

if (yaml-cpp_FOUND)
  set (yaml-cpp_LIBRARIES ${yaml-cpp_LIBRARY})
  set (yaml-cpp_INCLUDE_DIRS ${yaml-cpp_INCLUDE_DIR})
  if (NOT (TARGET yaml-cpp::yaml-cpp))
    add_library (yaml-cpp::yaml-cpp UNKNOWN IMPORTED)

    set_target_properties (yaml-cpp::yaml-cpp
      PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES ${yaml-cpp_INCLUDE_DIRS})
    if (EXISTS "${yaml-cpp_LIBRARY}")
      set_target_properties (yaml-cpp::yaml-cpp
        PROPERTIES
          IMPORTED_LOCATION "${yaml-cpp_LIBRARY}")
    endif ()
    foreach (build "RELEASE" "DEBUG")
      if (yaml-cpp_LIBRARY_${build})
        set_property (TARGET yaml-cpp::yaml-cpp APPEND PROPERTY
          IMPORTED_CONFIGURATIONS "${build}")
        set_target_properties (yaml-cpp::yaml-cpp PROPERTIES
          IMPORTED_LOCATION_${build} "${yaml-cpp_LIBRARY_${build}}")
      endif ()
    endforeach ()
  endif ()
endif ()
