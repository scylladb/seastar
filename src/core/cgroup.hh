/*
 * This file is open source software, licensed to you under the terms
 * of the Apache License, Version 2.0 (the "License").  See the NOTICE file
 * distributed with this work for additional information regarding copyright
 * ownership.  You may not use this file except in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
/*
 * Copyright (C) 2019 ScyllaDB
 */

#pragma once

#include <string>
#include <seastar/util/std-compat.hh>
#include <set>

namespace seastar {

namespace cgroup {

using std::optional;
using cpuset = std::set<unsigned>;

optional<cpuset> cpu_set();
size_t memory_limit();

template <typename T>
optional<T> read_setting_as(std::string path);

template <typename T>
optional<T> read_setting_V1V2_as(std::string cg1_path, std::string cg2_fname);
}

}
