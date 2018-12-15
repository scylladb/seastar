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

find_path (numactl_INCLUDE_DIR
  NAMES numaif.h)

find_library (numactl_LIBRARY
  NAMES numa)

mark_as_advanced (
  numactl_INCLUDE_DIR
  numactl_LIBRARY)

include (FindPackageHandleStandardArgs)

find_package_handle_standard_args (numactl
  REQUIRED_VARS
    numactl_LIBRARY
    numactl_INCLUDE_DIR)

set (numactl_INCLUDE_DIRS ${numactl_INCLUDE_DIR})
set (numactl_LIBRARIES ${numactl_LIBRARY})

if (numactl_FOUND AND NOT (TARGET numactl::numactl))
  add_library (numactl::numactl UNKNOWN IMPORTED)

  set_target_properties (numactl::numactl
    PROPERTIES
      IMPORTED_LOCATION ${numactl_LIBRARIES}
      INTERFACE_INCLUDE_DIRECTORIES ${numactl_INCLUDE_DIRS})
endif ()
