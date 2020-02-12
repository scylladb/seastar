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

#pragma once

#include <seastar/core/future.hh>
#include <seastar/core/file.hh>
#include <seastar/util/std-compat.hh>

namespace seastar {

const char* default_tmpdir();

class tmp_file {
    compat::filesystem::path _path;
    file _file;
    bool _is_open = false;

    static_assert(std::is_nothrow_constructible<compat::filesystem::path>::value,
        "filesystem::path's constructor must not throw");
    static_assert(std::is_nothrow_move_constructible<compat::filesystem::path>::value,
        "filesystem::path's move constructor must not throw");
public:
    tmp_file() noexcept = default;
    tmp_file(const tmp_file&) = delete;
    tmp_file(tmp_file&& x) noexcept;

    tmp_file& operator=(tmp_file&&) noexcept = default;

    ~tmp_file();

    future<> open(compat::filesystem::path path_template = default_tmpdir(),
            open_flags oflags = open_flags::rw,
            file_open_options options = {}) noexcept;
    future<> close() noexcept;
    future<> remove() noexcept;

    template <typename Func>
    static future<> do_with(compat::filesystem::path path_template, Func&& func,
            open_flags oflags = open_flags::rw,
            file_open_options options = {}) noexcept {
        static_assert(std::is_nothrow_move_constructible<Func>::value,
            "Func's move constructor must not throw");
        return seastar::do_with(tmp_file(), [func = std::move(func), path_template = std::move(path_template), oflags, options = std::move(options)] (tmp_file& t) mutable {
            return t.open(std::move(path_template), oflags, std::move(options)).then([&t, func = std::move(func)] () mutable {
                return func(t);
            }).finally([&t] {
                return t.close().finally([&t] {
                    return t.remove();
                });
            });
        });
    }

    template <typename Func>
    static future<> do_with(Func&& func) noexcept {
        return do_with(default_tmpdir(), std::move(func));
    }

    bool has_path() const {
        return !_path.empty();
    }

    bool is_open() const {
        return _is_open;
    }

    const compat::filesystem::path& get_path() const {
        return _path;
    }

    file& get_file() {
        return _file;
    }
};

/// Returns a future for an opened tmp_file exclusively created by the function.
///
/// \param path_template - path where the file is to be created,
///                        optionally including a template for the file name.
/// \param open_flags - optional open flags (open_flags::create | open_flags::exclusive are added to those by default)
/// \param file_open_options - additional options, e.g. for setting the created file permission.
///
/// \note
///    path_template may optionally include a filename template in the last component of the path.
///    The template is indicated by two or more consecutive XX's.
///    Those will be replaced in the result path by a unique string.
///
///    If no filename template is found, then path_template is assumed to refer to the directory where
///    the temporary file is to be created at (a.k.a. the parent directory) and `default_tmp_name_template`
///    is appended to the path as the filename template.
///
///    The parent directory must exist and be writable to the current process.
///
future<tmp_file> make_tmp_file(compat::filesystem::path path_template = default_tmpdir(),
        open_flags oflags = open_flags::rw, file_open_options options = {}) noexcept;

} // namespace seastar
