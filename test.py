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
import subprocess
import seastar_cmake

if __name__ == "__main__":

    parser = argparse.ArgumentParser(description="Seastar test runner")
    parser.add_argument('--fast',  action="store_true", help="Run only fast tests")
    parser.add_argument('--name',  action="store", help="Run only test whose name contains given string")
    parser.add_argument('--mode', choices=seastar_cmake.SUPPORTED_MODES, help="Run only tests for given build mode")
    parser.add_argument('--timeout', action="store",default="300",type=int, help="timeout value for test execution")
    parser.add_argument('--jenkins', action="store",help="jenkins output file prefix")
    parser.add_argument('--smp', '-c', action="store",default='2',type=int,help="Number of threads for multi-core tests")
    parser.add_argument('--verbose', '-v', action = 'store_true', default = False,
                        help = 'Verbose reporting')
    args = parser.parse_args()

    MODES = [args.mode] if args.mode else seastar_cmake.SUPPORTED_MODES

    def run_tests(mode):
        BUILD_PATH = seastar_cmake.BUILD_PATHS[mode]

        # For convenience.
        tr = seastar_cmake.translate_arg

        TRANSLATED_CMAKE_ARGS = [
            tr(args.timeout, 'TEST_TIMEOUT'),
            tr(args.fast, 'EXECUTE_ONLY_FAST_TESTS'),
            tr(args.smp, 'UNIT_TEST_SMP'),
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

        CTEST_ARGS = ['ctest', BUILD_PATH] + TRANSLATED_CTEST_ARGS
        print(CTEST_ARGS)
        subprocess.check_call(CTEST_ARGS, shell=False, cwd=BUILD_PATH)

    for mode in MODES:
        run_tests(mode)
