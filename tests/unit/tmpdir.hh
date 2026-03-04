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
 * Copyright (C) 2020 ScyllaDB Ltd.
 */

#pragma once

#include <seastar/util/tmp_file.hh>

namespace seastar {

/**
 * Temp dir helper for RAII usage when doing tests
 * in seastar threads. Will not work in "normal" mode.
 * Just use tmp_dir::do_with for that.
 */
class tmpdir {
    seastar::tmp_dir _tmp;
public:
    tmpdir(tmpdir&&) = default;
    tmpdir(const tmpdir&) = delete;

    tmpdir(const sstring& name = sstring(seastar::default_tmpdir()) + "/testXXXX") {
        _tmp.create(std::filesystem::path(name)).get();
    }
    ~tmpdir() {
        _tmp.remove().get();
    }
    auto path() const {
        return _tmp.get_path();
    }
};

}
