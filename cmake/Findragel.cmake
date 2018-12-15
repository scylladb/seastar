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

find_program (
  ragel_RAGEL_EXECUTABLE
  ragel)

mark_as_advanced (ragel_RAGEL_EXECUTABLE)

set (_ragel_version_pattern "[0-9]+\\.[0-9]+\\.[0-9]+(\\.[0-9]+)?")

if (ragel_RAGEL_EXECUTABLE)
  set (ragel_FOUND ON)

  exec_program (${ragel_RAGEL_EXECUTABLE}
    ARGS -v
    OUTPUT_VARIABLE _ragel_version_output)

  if (${_ragel_version_output} MATCHES "version (${_ragel_version_pattern})")
    set (ragel_VERSION ${CMAKE_MATCH_1})
  endif ()
endif ()

find_package_handle_standard_args (ragel
  REQUIRED_VARS ragel_RAGEL_EXECUTABLE
  VERSION_VAR ragel_VERSION)
