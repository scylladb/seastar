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
# Copyright (C) 2026 ScyllaDB
#

include (FindPackageHandleStandardArgs)

find_package (PkgConfig QUIET)

if (PkgConfig_FOUND)
  pkg_check_modules (PC_lttng-ust QUIET lttng-ust)
endif ()

find_path (LTTngUST_INCLUDE_DIR
  NAMES lttng/tracepoint.h
  HINTS ${PC_lttng-ust_INCLUDE_DIRS})

find_library (LTTngUST_LIBRARY
  NAMES lttng-ust
  HINTS ${PC_lttng-ust_LIBRARY_DIRS})

mark_as_advanced (
  LTTngUST_INCLUDE_DIR
  LTTngUST_LIBRARY)

find_package_handle_standard_args (LTTngUST
  REQUIRED_VARS
    LTTngUST_INCLUDE_DIR
    LTTngUST_LIBRARY)

if (LTTngUST_FOUND AND NOT TARGET LTTng::UST)
  add_library (LTTng::UST UNKNOWN IMPORTED)
  set_target_properties (LTTng::UST
    PROPERTIES
      IMPORTED_LOCATION ${LTTngUST_LIBRARY}
      INTERFACE_INCLUDE_DIRECTORIES ${LTTngUST_INCLUDE_DIR})
endif ()
