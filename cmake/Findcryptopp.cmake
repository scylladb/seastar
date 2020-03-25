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

find_library (cryptopp_LIBRARY
  NAMES cryptopp)

find_path (cryptopp_INCLUDE_DIR
  NAMES cryptopp/aes.h
  PATH_SUFFIXES cryptopp)

mark_as_advanced (
  cryptopp_LIBRARY
  cryptopp_INCLUDE_DIR)

include (FindPackageHandleStandardArgs)

find_package_handle_standard_args (cryptopp
  REQUIRED_VARS
    cryptopp_LIBRARY
    cryptopp_INCLUDE_DIR)

set (cryptopp_LIBRARIES ${cryptopp_LIBRARY})
set (cryptopp_INCLUDE_DIRS ${cryptopp_INCLUDE_DIR})

if (cryptopp_FOUND AND NOT (TARGET cryptopp::cryptopp))
  add_library (cryptopp::cryptopp UNKNOWN IMPORTED)

  set_target_properties (cryptopp::cryptopp
    PROPERTIES
      IMPORTED_LOCATION ${cryptopp_LIBRARIES}
      INTERFACE_INCLUDE_DIRECTORIES ${cryptopp_INCLUDE_DIRS})
endif ()
