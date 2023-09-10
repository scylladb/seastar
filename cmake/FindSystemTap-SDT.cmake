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

find_path (SystemTap-SDT_INCLUDE_DIR
  NAMES sys/sdt.h)

mark_as_advanced (
  SystemTap-SDT_INCLUDE_DIR)

include (FindPackageHandleStandardArgs)

find_package_handle_standard_args (SystemTap-SDT
  REQUIRED_VARS SystemTap-SDT_INCLUDE_DIR)

if (NOT TARGET SystemTap::SDT)
  add_library (SystemTap::SDT INTERFACE IMPORTED)
  set_target_properties (SystemTap::SDT
    PROPERTIES
      INTERFACE_INCLUDE_DIRECTORIES ${SystemTap-SDT_INCLUDE_DIR})
endif ()
