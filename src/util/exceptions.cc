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
 * Copyright 2020 ScyllaDB
 */

#include <seastar/util/exceptions.hh>

namespace seastar {

std::filesystem::filesystem_error make_filesystem_error(const std::string& what, std::filesystem::path path, int error) {
    return std::filesystem::filesystem_error(what, std::move(path), std::error_code(error, std::system_category()));
}

std::filesystem::filesystem_error make_filesystem_error(const std::string& what, std::filesystem::path path1, std::filesystem::path path2, int error) {
    return std::filesystem::filesystem_error(what, std::move(path1), std::move(path1), std::error_code(error, std::system_category()));
}

} // namespace seastar
