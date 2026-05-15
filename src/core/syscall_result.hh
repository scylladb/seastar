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
 * Copyright 2019 ScyllaDB
 */

namespace seastar {

namespace fs = std::filesystem;

#pragma once
template <typename T>
struct syscall_result {
    T result;
    int error;
    syscall_result(T result, int error) : result{std::move(result)}, error{error} {
    }

    bool failed() const noexcept {
        return long(result) == -1;
    }

    void throw_if_error() const {
        if (failed()) {
            throw std::system_error(ec());
        }
    }

    // Build (without throwing) an exception_ptr describing a system error
    // for this syscall result. Intended for use in coroutines, paired with
    // `co_return coroutine::exception(...)` (for future<T>) or
    // `co_await coroutine::return_exception_ptr(...)` (for future<>), so the
    // failure is propagated to the awaiter without invoking the throw/unwind
    // machinery.
    std::exception_ptr make_system_error_ptr() const noexcept {
        return std::make_exception_ptr(std::system_error(ec()));
    }

    std::exception_ptr make_fs_exception_ptr(const sstring& reason, const fs::path& path) const {
        return std::make_exception_ptr(fs::filesystem_error(reason, path, ec()));
    }

    std::exception_ptr make_fs_exception_ptr(const sstring& reason, const fs::path& path1, const fs::path& path2) const {
        return std::make_exception_ptr(fs::filesystem_error(reason, path1, path2, ec()));
    }

    std::error_code ec() const {
        return std::error_code(error, std::system_category());
    }
};

// Wrapper for a system call result containing the return value,
// an output parameter that was returned from the syscall, and errno.
template <typename Extra>
struct syscall_result_extra : public syscall_result<int> {
    Extra extra;
    syscall_result_extra(int result, int error, Extra e) : syscall_result<int>{result, error}, extra{std::move(e)} {
    }
};

template <typename T>
syscall_result<T>
wrap_syscall(T result) {
    return syscall_result<T>{std::move(result), errno};
}

template <typename Extra>
syscall_result_extra<Extra>
wrap_syscall(int result, const Extra& extra) {
    return syscall_result_extra<Extra>{result, errno, extra};
}

}
