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
# Copyright (C) 2023 Scylladb, Ltd.
#

# Try to compile without the library first.
include (CheckFunctionExists)
check_function_exists (getcontext
  ucontext_NO_EXPLICIT_LINK)

if (ucontext_NO_EXPLICIT_LINK)
  set (ucontext_FOUND yes)
else ()
  # The `libucontext` library is required.
  find_package (PkgConfig QUIET REQUIRED)
  pkg_check_modules (PC_ucontext QUIET ucontext)
  find_library (ucontext_LIBRARY
    NAMES ucontext
    HINTS
      ${PC_ucontext_LIBDIR}
      ${PC_ucontext_LIBRARY_DIRS})
  mark_as_advanced (ucontext_LIBRARY)
  include (FindPackageHandleStandardArgs)
  find_package_handle_standard_args (ucontext
    REQUIRED_VARS ucontext_LIBRARY)
endif ()

if (ucontext_FOUND AND NOT (TARGET ucontext::ucontext))
  add_library (ucontext::ucontext INTERFACE IMPORTED)

  set_target_properties (ucontext::ucontext
    PROPERTIES
      INTERFACE_LINK_LIBRARIES "${ucontext_LIBRARY}")
endif ()
