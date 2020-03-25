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
# Copyright (C) 2019 Scylladb, Ltd.
#

function (_stdatomic_can_link var)
  include (CheckCXXSourceCompiles)
  set (test_code "int main() {}")
  set (CMAKE_REQUIRED_LIBRARIES -latomic)
  check_cxx_source_compiles ("${test_code}" ${var})
endfunction ()

_stdatomic_can_link (StdAtomic_EXPLICIT_LINK)

#
# If linking against `-latomic` is successful, then do it unconditionally.
#

if (StdAtomic_EXPLICIT_LINK)
  set (StdAtomic_LIBRARY_NAME atomic)
  set (StdAtomic_LIBRARIES -l${StdAtomic_LIBRARY_NAME})
  include (FindPackageHandleStandardArgs)

  find_package_handle_standard_args (StdAtomic
    REQUIRED_VARS StdAtomic_LIBRARIES)
endif ()

if (NOT (TARGET StdAtomic::atomic))
  add_library (StdAtomic::atomic INTERFACE IMPORTED)

  set_target_properties (StdAtomic::atomic
    PROPERTIES
      INTERFACE_LINK_LIBRARIES "${StdAtomic_LIBRARIES}")
endif ()
