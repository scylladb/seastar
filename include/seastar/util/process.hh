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
 * Copyright (C) 2022 Kefu Chai ( tchaikov@gmail.com )
 */

#pragma once

#include <sys/types.h>
#include <algorithm>
#include <filesystem>
#include <initializer_list>
#include <iterator>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>
#include <fmt/format.h>
#include <seastar/core/iostream.hh>
#include <seastar/core/posix.hh>
#include <seastar/core/sstring.hh>

namespace seastar::experimental {

/// The optional parameters for spawning a subprocess
///
/// \note see \c execve(2) for more details on \c argv and \c env.
struct spawn_parameters {
    /// The arguments passed to the program
    std::vector<sstring> argv;
    /// The environment variables for the program
    std::vector<sstring> env;
};

/// Interact with a spawned subprocess
///
/// \note the spawned subprocess should always be \c wait()'ed. Otherwise,
/// the Seastar application spawning the subprocess will leave us with
/// one ore more zombie subprocesses after it exists.
class process {
    struct create_tag {};
    /// Spawn a subprocess using \c posix_spawn(3)
    ///
    /// \param pathname the full path to the executable
    /// \param params parameters for spawning the subprocess
    ///
    /// \returns a \c process instance representing the spawned subprocess
    static future<process> spawn(const std::filesystem::path& pathname,
                                 spawn_parameters params);
    /// Spawn a subprocess using \c posix_spawn(3)
    ///
    /// \param pathname the full path to the executable
    ///
    /// \returns a \c process instance representing the spawned subprocess
    static future<process> spawn(const std::filesystem::path& pathname);
public:
    process(create_tag, pid_t pid, file_desc&& stdin, file_desc&& stdout, file_desc&& stderr);
    /// Return an writable stream which provides input from the child process
    output_stream<char> stdin();
    /// Return an writable stream which provides stdout output from the child process
    input_stream<char> stdout();
    /// Return an writable stream which provides stderr output from the child process
    input_stream<char> stderr();
    struct wait_exited {
        int exit_code;
    };
    struct wait_signaled {
        int terminating_signal;
    };
    using wait_status = std::variant<wait_exited, wait_signaled>;
    /// Wait until the child process exits or terminates
    ///
    /// \returns the exit status
    future<wait_status> wait();
    /// Stop the process using SIGTERM
    void terminate();
    /// Force the process to exit using SIGKILL
    void kill();

private:
    const pid_t _pid;
    file_desc _stdin;
    file_desc _stdout;
    file_desc _stderr;

    friend future<process> spawn_process(const std::filesystem::path&,
                                         spawn_parameters);
    friend future<process> spawn_process(const std::filesystem::path&);
};
}
