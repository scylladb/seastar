#!/usr/bin/env python3
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

import argparse
import os
import subprocess
import seastar_cmake

if __name__ == "__main__":

    parser = argparse.ArgumentParser(description="Seastar test runner")
    parser.add_argument('--fast',  action="store_true", help="Run only fast tests")
    parser.add_argument('--name',  action="store", help="Run only test whose name contains given string")
    parser.add_argument('--mode', choices=seastar_cmake.SUPPORTED_MODES, help="Run only tests for given build mode")
    parser.add_argument('--build-root', action='store', default=seastar_cmake.DEFAULT_BUILD_ROOT, type=str,
                        help="The name of the build root build directoy: "
                        "using a different name allows multiple configurations to co-exist in the same repository")
    parser.add_argument('--timeout', action="store",default="300",type=int, help="timeout value for test execution")
    parser.add_argument('--jenkins', action="store",help="jenkins output file prefix")
    parser.add_argument('--smp', '-c', action="store",default='2',type=int,help="Number of threads for multi-core tests")
    parser.add_argument('--reactor-backend', action="store", default='',
                        help="Run the tests with this reactor backend (io_uring, asymmetric_io_uring, "
                             "linux-aio or epoll) instead of the one the reactor picks by default. Tests "
                             "which pass --reactor-backend themselves are left on their own choice.")
    parser.add_argument('--async-workers-cpuset', action="store", default='',
                        help="CPUs for the asymmetric_io_uring async workers, which that backend requires. "
                             "Defaults to the highest CPU id this process may run on.")
    parser.add_argument('--verbose', '-v', action = 'store_true', default = False,
                        help = 'Verbose reporting')
    parser.add_argument('--offline', action="store_true", default = False,
                        help="Disable tests accessing internet")
    parser.add_argument('ctest_forward', nargs='*', help="These parameters will be passed directly to ctest")
    args = parser.parse_args()

    MODES = [args.mode] if args.mode else seastar_cmake.SUPPORTED_MODES

    def run_tests(mode):
        BUILD_PATH = seastar_cmake.build_path(mode, args.build_root)

        # For convenience.
        tr = seastar_cmake.translate_arg

        TRANSLATED_CMAKE_ARGS = [
            tr(args.timeout, 'TEST_TIMEOUT'),
            tr(args.fast, 'EXECUTE_ONLY_FAST_TESTS'),
            tr(args.smp, 'UNIT_TEST_SMP'),
            tr(not args.offline, 'ENABLE_TESTS_ACCESSING_INTERNET'),
            tr(args.jenkins, 'JENKINS', value_when_none=''),
        ]

        # Modify the existing build by pointing to the build directory.
        CMAKE_ARGS = ['cmake', BUILD_PATH] + TRANSLATED_CMAKE_ARGS
        print(CMAKE_ARGS)
        subprocess.check_call(CMAKE_ARGS, shell=False, cwd=seastar_cmake.ROOT_PATH)

        TRANSLATED_CTEST_ARGS = ['--output-on-failure']
        if args.verbose:
            TRANSLATED_CTEST_ARGS += ['--verbose']
        if args.name:
            TRANSLATED_CTEST_ARGS += ['-R', args.name]

        # Seastar reads options from SEASTAR_OPTIONS, which a test's own arguments
        # override, so this leaves the tests which pin a backend on the one they
        # asked for. It also reaches the helper binaries that some tests spawn
        # themselves, which arguments added here would not.
        seastar_options = []
        if args.reactor_backend:
            seastar_options += ['--reactor-backend', args.reactor_backend]
        cpuset = args.async_workers_cpuset
        if not cpuset and args.reactor_backend == 'asymmetric_io_uring':
            # That backend refuses to start without CPUs for its async workers.
            # Hand it the highest CPU id we may use and leave the rest to the shards.
            cpuset = str(max(os.sched_getaffinity(0)))
        if cpuset:
            seastar_options += ['--async-workers-cpuset', cpuset]

        TEST_ENV = dict(os.environ)
        if seastar_options:
            # Appended, so anything the caller already set is kept. Setting the
            # same option in both places is an error, which is the right answer.
            TEST_ENV['SEASTAR_OPTIONS'] = ' '.join(
                filter(None, [TEST_ENV.get('SEASTAR_OPTIONS', '')] + seastar_options))

        CTEST_ARGS = ['ctest', BUILD_PATH] + TRANSLATED_CTEST_ARGS + args.ctest_forward
        print(CTEST_ARGS)
        subprocess.check_call(CTEST_ARGS, shell=False, cwd=BUILD_PATH, env=TEST_ENV)

    for mode in MODES:
        run_tests(mode)
