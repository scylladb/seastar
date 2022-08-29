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
# Copyright 2022 Jinyong Ha (jyha200@gmail.com), Heewon Shin (shw096@snu.ac.kr)
#

find_package (PkgConfig REQUIRED)

pkg_search_module (NVME libnvme)
INCLUDE(CheckIncludeFile)

find_library (NVME_LIBRARY
  NAMES nvme
  HINTS
    ${NVME_PC_LIBDIR}
    ${NVME_PC_LIBRARY_DIRS})

find_path (NVME_INCLUDE_DIR
  NAMES libnvme.h
  HINTS
    ${NVME_PC_INCLUDE_DIR}
    ${NVME_PC_INCLUDE_DIRS})

if (NVME_INCLUDE_DIR)
  include (CheckStructHasMember)
  include (CMakePushCheckState)
  cmake_push_check_state (RESET)
  list(APPEND CMAKE_REQUIRED_INCLUDES ${NVME_INCLUDE_DIR})
  set (HAVE_LIBNVME TRUE)
endif ()

mark_as_advanced (
  NVME_LIBRARY
  NVME_INCLUDE_DIR
  HAVE_LIBNVME)


include (FindPackageHandleStandardArgs)

find_package_handle_standard_args (LibNvme
  REQUIRED_VARS
    NVME_LIBRARY
    NVME_INCLUDE_DIR
  VERSION_VAR NVME_PC_VERSION)

set (NVME_LIBRARIES ${NVME_LIBRARY})
set (NVME_INCLUDE_DIRS ${NVME_INCLUDE_DIR})

if (NVME_FOUND AND NOT (TARGET NVME::nvme))
  add_library (NVME::nvme UNKNOWN IMPORTED)

  set_target_properties (NVME::nvme
    PROPERTIES
      IMPORTED_LOCATION ${NVME_LIBRARY}
      INTERFACE_INCLUDE_DIRECTORIES ${NVME_INCLUDE_DIRS})
endif ()
