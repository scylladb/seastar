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

function (seastar_jenkins_arguments test_name var)
  string (TOLOWER ${CMAKE_BUILD_TYPE} mode)
  set (output_file "${CMAKE_SOURCE_DIR}/${Seastar_JENKINS}.${mode}.${test_name}_test.boost.xml")

  set (${var}
    --output_format=XML
    --log_level=all
    --report_level=no
    --log_sink=${output_file}
    PARENT_SCOPE)
endfunction ()

# Performance tests.
add_subdirectory (perf)

# Unit tests.
add_subdirectory (unit)
