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

set (_dl_test_source ${CMAKE_CURRENT_LIST_DIR}/code_tests/dl_test.cc)

# Try to compile without the library first.
try_compile (_dl_test_nil
  ${CMAKE_CURRENT_BINARY_DIR}
  SOURCES ${_dl_test_source})

if (NOT _dl_test_nil)
  # The `dl` library is required.

  try_compile (_dl_test
    ${CMAKE_CURRENT_BINARY_DIR}
    SOURCES ${_dl_test_source}
    LINK_LIBRARIES dl)

  if (_dl_test)
    set (dl_LIBRARY_NAME dl)
  endif ()
endif ()

if (_dl_test_nil OR dl_LIBRARY_NAME)
  set (dl_FOUND ON)
  set (dl_LIBRARIES -l${dl_LIBRARY_NAME})
endif ()

include (FindPackageHandleStandardArgs)

find_package_handle_standard_args (dl
  FOUND_VAR dl_FOUND
  REQUIRED_VARS dl_LIBRARY_NAME)

if (dl_FOUND AND NOT (TARGET dl::dl))
  add_library (dl::dl INTERFACE IMPORTED)

  set_target_properties (dl::dl
    PROPERTIES
      INTERFACE_LINK_LIBRARIES ${dl_LIBRARIES})
endif ()

mark_as_advanced (dl_LIBRARY_NAME)
