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

include (CheckCXXSourceCompiles)
file (READ ${CMAKE_CURRENT_LIST_DIR}/code_tests/StdFilesystem_test.cc _stdfilesystem_test_code)

macro (_stdfilesystem_check_compiles var)
  set (libraries ${ARGN})
  set (CMAKE_REQUIRED_LIBRARIES ${libraries})
  set (CMAKE_REQUIRED_FLAGS -std=gnu++14)
  check_cxx_source_compiles ("${_stdfilesystem_test_code}" ${var})
endmacro ()

# Try to compile without the library first.
_stdfilesystem_check_compiles (StdFilesystem_NO_EXPLICIT_LINK)

if (StdFilesystem_NO_EXPLICIT_LINK)
  set (StdFilesystem_FOUND yes)
else ()
  _stdfilesystem_check_compiles (StdFilesystem_STDCXXFS_LIBRARY
    stdc++fs)

  if (StdFilesystem_STDCXXFS_LIBRARY)
    set (StdFilesystem_LIBRARY_NAME stdc++fs)
  else ()
    # Try libc++.
    _stdfilesystem_check_compiles (StdFilesystem_CXXEXPERIMENTAL_LIBRARY
      libc++experimental)

    if (StdFilesystem_CXXEXPERIMENTAL_LIBRARY)
      set (StdFilesystem_LIBRARY_NAME c++experimental)
    endif ()
  endif ()

  if (StdFilesystem_LIBRARY_NAME)
    set (StdFilesystem_LIBRARIES -l${StdFilesystem_LIBRARY_NAME})
  endif ()

  include (FindPackageHandleStandardArgs)

  find_package_handle_standard_args (StdFilesystem
    REQUIRED_VARS StdFilesystem_LIBRARY_NAME)
endif ()

if (StdFilesystem_FOUND AND NOT (TARGET StdFilesystem::filesystem))
  add_library (StdFilesystem::filesystem INTERFACE IMPORTED)

  set_target_properties (StdFilesystem::filesystem
    PROPERTIES
      INTERFACE_LINK_LIBRARIES ${StdFilesystem_LIBRARIES})
endif ()
