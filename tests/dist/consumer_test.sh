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

# This test expects the following environmental variables to be defined:
#
#     CONSUMER_SOURCE_DIR
#     SEASTAR_SOURCE_DIR
#

set -e

cd "${CONSUMER_SOURCE_DIR}"
./cooking.sh -r test_dist -t Release

#
# Consume from CMake.
#

cmake --build build
build/cmake_consumer
build/cmake_testing_consumer

ingredients_dir="build/_cooking/installed"
library_path="${ingredients_dir}"/lib

#
# Consume Seastar from its build directory with pkg-config.
#

pkg_config_path="build/_cooking/ingredient/Seastar/build"
make BUILD_DIR=build-no-inst PKG_CONFIG_PATH="${pkg_config_path}"
LD_LIBRARY_PATH="${library_path}" build-no-inst/pkgconfig_consumer
LD_LIBRARY_PATH="${library_path}" build-no-inst/pkgconfig_testing_consumer

#
# Consume Seastar installed to the file-system, with pkg-config.
#

pkg_config_path="${library_path}"/pkgconfig
make BUILD_DIR=build PKG_CONFIG_PATH="${pkg_config_path}"
LD_LIBRARY_PATH="${library_path}" build/pkgconfig_consumer
LD_LIBRARY_PATH="${library_path}" build/pkgconfig_testing_consumer
