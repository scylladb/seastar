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
 * Copyright (C) 2021 ScyllaDB
 */

#pragma once

namespace seastar::internal {

class source_location {
    const char* _file;
    const char* _func;
    int _line;
    int _col;

    constexpr source_location(const char* file, const char* func, int line, int col) noexcept
        : _file(file)
        , _func(func)
        , _line(line)
        , _col(col)
    { }
public:
    constexpr source_location() noexcept
        : _file("unknown")
        , _func(_file)
        , _line(0)
        , _col(0)
    { }

    static source_location current(const char* file = __builtin_FILE(), const char* func = __builtin_FUNCTION(), int line = __builtin_LINE(), int col = 0) noexcept {
        return source_location(file, func, line, col);
    }

    constexpr const char* file_name() const noexcept { return _file; }
    constexpr const char* function_name() const noexcept { return _func; }
    constexpr int line() const noexcept { return _line; }
    constexpr int column() const noexcept { return _col; }
};

} // namespace seastar::internal
