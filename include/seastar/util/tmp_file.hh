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
#include <seastar/core/thread.hh>
#include <seastar/util/std-compat.hh>

namespace seastar {

const std::filesystem::path& default_tmpdir();
void set_default_tmpdir(std::filesystem::path);

class tmp_file {
    std::filesystem::path _path;
    file _file;
    bool _is_open = false;

    static_assert(std::is_nothrow_constructible<std::filesystem::path>::value,
        "filesystem::path's constructor must not throw");
    static_assert(std::is_nothrow_move_constructible<std::filesystem::path>::value,
        "filesystem::path's move constructor must not throw");
public:
    tmp_file() noexcept = default;
    tmp_file(const tmp_file&) = delete;
    tmp_file(tmp_file&& x) noexcept;

    tmp_file& operator=(tmp_file&&) noexcept = default;

    ~tmp_file();

    future<> open(std::filesystem::path path_template = default_tmpdir(),
            open_flags oflags = open_flags::rw,
            file_open_options options = {}) noexcept;
    future<> close() noexcept;
    future<> remove() noexcept;

    template <typename Func>
    static future<> do_with(std::filesystem::path path_template, Func&& func,
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

    const std::filesystem::path& get_path() const {
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
/// \param oflags - optional \ref open_flags (open_flags::create | open_flags::exclusive are added to those by default)
/// \param options - additional \ref file_open_options, e.g. for setting the created file permission.
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
future<tmp_file> make_tmp_file(std::filesystem::path path_template = default_tmpdir(),
        open_flags oflags = open_flags::rw, file_open_options options = {}) noexcept;

class tmp_dir {
    std::filesystem::path _path;

public:
    tmp_dir() = default;
    tmp_dir(const tmp_dir&) = delete;
    tmp_dir(tmp_dir&& x) = default;

    tmp_dir& operator=(tmp_dir&&) noexcept = default;

    ~tmp_dir();

    future<> create(std::filesystem::path path_template = default_tmpdir(),
            file_permissions create_permissions = file_permissions::default_dir_permissions) noexcept;
    future<> remove() noexcept;

    template <typename Func>
    SEASTAR_CONCEPT( requires std::is_nothrow_move_constructible_v<Func> )
    static future<> do_with(std::filesystem::path path_template, Func&& func,
            file_permissions create_permissions = file_permissions::default_dir_permissions) noexcept {
        static_assert(std::is_nothrow_move_constructible_v<Func>,
            "Func's move constructor must not throw");
        return seastar::do_with(tmp_dir(), [func = std::move(func), path_template = std::move(path_template), create_permissions] (tmp_dir& t) mutable {
            return t.create(std::move(path_template), create_permissions).then([&t, func = std::move(func)] () mutable {
                return func(t);
            }).finally([&t] {
                return t.remove();
            });
        });
    }

    template <typename Func>
    static future<> do_with(Func&& func) noexcept {
        return do_with(default_tmpdir(), std::move(func));
    }

    template <typename Func>

    SEASTAR_CONCEPT( requires std::is_nothrow_move_constructible_v<Func> )
    static future<> do_with_thread(Func&& func) noexcept {
        static_assert(std::is_nothrow_move_constructible_v<Func>,
            "Func's move constructor must not throw");
        return async([func = std::move(func)] () mutable {
            auto t = tmp_dir();
            t.create().get();
            futurize_invoke(func, t).finally([&t] {
                return t.remove();
            }).get();
        });
    }

    bool has_path() const {
        return !_path.empty();
    }

    const std::filesystem::path& get_path() const {
        return _path;
    }
};

/// Returns a future for a tmp_dir exclusively created by the function.
///
/// \param path_template - path where the file is to be created,
///                        optionally including a template for the file name.
/// \param create_permissions - optional permissions for the newly created directory.
///
/// \note
///    path_template may optionally include a name template in the last component of the path.
///    The template is indicated by two or more consecutive XX's.
///    Those will be replaced in the result path by a unique string.
///
///    If no name template is found, then path_template is assumed to refer to the directory where
///    the temporary dir is to be created at (a.k.a. the parent directory) and `default_tmp_name_template`
///    is appended to the path as the name template for the to-be-created directory.
///
///    The parent directory must exist and be writable to the current process.
///
future<tmp_dir> make_tmp_dir(std::filesystem::path path_template = default_tmpdir(),
        file_permissions create_permissions = file_permissions::default_dir_permissions) noexcept;

} // namespace seastar
