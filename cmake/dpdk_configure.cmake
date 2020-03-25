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

file (READ ${Seastar_DPDK_CONFIG_FILE_IN} dpdk_config)
file (STRINGS ${Seastar_DPDK_CONFIG_FILE_CHANGES} dpdk_config_changes)
set (word_pattern "[^\n\r \t]+")

foreach (var ${dpdk_config_changes})
  if (var MATCHES "(${word_pattern})=(${word_pattern})")
    set (key ${CMAKE_MATCH_1})
    set (value ${CMAKE_MATCH_2})

    string (REGEX REPLACE
      "${key}=${word_pattern}"
      "${key}=${value}"
      dpdk_config
      ${dpdk_config})
  endif ()
endforeach ()

file (WRITE ${Seastar_DPDK_CONFIG_FILE_OUT} ${dpdk_config})
file (APPEND ${Seastar_DPDK_CONFIG_FILE_OUT} "CONFIG_RTE_MACHINE=${Seastar_DPDK_MACHINE}")
