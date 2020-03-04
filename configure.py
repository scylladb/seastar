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
import distutils.dir_util
import os
import seastar_cmake
import subprocess
import sys
import tempfile

tempfile.tempdir = "./build/tmp"

def add_tristate(arg_parser, name, dest, help):
    arg_parser.add_argument('--enable-' + name, dest = dest, action = 'store_true', default = None,
                            help = 'Enable ' + help)
    arg_parser.add_argument('--disable-' + name, dest = dest, action = 'store_false', default = None,
                            help = 'Disable ' + help)

def try_compile(compiler, source = '', flags = []):
    return try_compile_and_link(compiler, source, flags = flags + ['-c'])

def ensure_tmp_dir_exists():
    if not os.path.exists(tempfile.tempdir):
        os.makedirs(tempfile.tempdir)

def try_compile_and_link(compiler, source = '', flags = []):
    ensure_tmp_dir_exists()
    with tempfile.NamedTemporaryFile() as sfile:
        ofile = tempfile.mktemp()
        try:
            sfile.file.write(bytes(source, 'utf-8'))
            sfile.file.flush()
            # We can't write to /dev/null, since in some cases (-ftest-coverage) gcc will create an auxiliary
            # output file based on the name of the output file, and "/dev/null.gcsa" is not a good name
            return subprocess.call([compiler, '-x', 'c++', '-o', ofile, sfile.name] + flags,
                                   stdout = subprocess.DEVNULL,
                                   stderr = subprocess.DEVNULL) == 0
        finally:
            if os.path.exists(ofile):
                os.unlink(ofile)
def dialect_supported(dialect, compiler='g++'):
    return try_compile(compiler=compiler, source='', flags=['-std=' + dialect])

arg_parser = argparse.ArgumentParser('Configure seastar')
arg_parser.add_argument('--mode', action='store', choices=seastar_cmake.SUPPORTED_MODES + ['all'], default='all')
arg_parser.add_argument('--cflags', action = 'store', dest = 'user_cflags', default = '',
                        help = 'Extra flags for the C++ compiler')
arg_parser.add_argument('--ldflags', action = 'store', dest = 'user_ldflags', default = '',
                        help = 'Extra flags for the linker')
arg_parser.add_argument('--optflags', action = 'store', dest = 'user_optflags', default = '',
                        help = 'Extra optimization flags for the release mode')
arg_parser.add_argument('--api-level', action='store', dest='api_level', default='2',
                        help='Compatibility API level (2=latest)')
arg_parser.add_argument('--compiler', action = 'store', dest = 'cxx', default = 'g++',
                        help = 'C++ compiler path')
arg_parser.add_argument('--c-compiler', action='store', dest='cc', default='gcc',
                        help = 'C compiler path (for bundled libraries such as dpdk)')
arg_parser.add_argument('--c++-dialect', action='store', dest='cpp_dialect', default='',
                        help='C++ dialect to build with [default: %(default)s]')
arg_parser.add_argument('--cook', action='append', dest='cook', default=[],
                        help='Supply this dependency locally for development via `cmake-cooking` (can be repeated)')
arg_parser.add_argument('--verbose', dest='verbose', action='store_true', help='Make configure output more verbose.')
add_tristate(
    arg_parser,
    name = 'dpdk',
    dest = 'dpdk',
    help = 'DPDK support')
add_tristate(
    arg_parser,
    name = 'hwloc',
    dest = 'hwloc',
    help = 'hwloc support')
add_tristate(
    arg_parser,
    name = 'alloc-failure-injector',
    dest = 'alloc_failure_injection',
    help = 'allocation failure injection')
add_tristate(
    arg_parser,
    name = 'task-backtrace',
    dest = 'task_backtrace',
    help = 'Collect backtrace at deferring points')
add_tristate(
    arg_parser,
    name = 'experimental-coroutines-ts',
    dest = "coroutines_ts",
    help = 'experimental support for Coroutines TS')
add_tristate(
    arg_parser,
    name = 'unused-result-error',
    dest = "unused_result_error",
    help = 'Make [[nodiscard]] violations an error')
arg_parser.add_argument('--allocator-page-size', dest='alloc_page_size', type=int, help='override allocator page size')
arg_parser.add_argument('--without-tests', dest='exclude_tests', action='store_true', help='Do not build tests by default')
arg_parser.add_argument('--without-apps', dest='exclude_apps', action='store_true', help='Do not build applications by default')
arg_parser.add_argument('--without-demos', dest='exclude_demos', action='store_true', help='Do not build demonstrations by default')
arg_parser.add_argument('--split-dwarf', dest='split_dwarf', action='store_true', default=False,
                        help='use of split dwarf (https://gcc.gnu.org/wiki/DebugFission) to speed up linking')
arg_parser.add_argument('--heap-profiling', dest='heap_profiling', action='store_true', default=False, help='Enable heap profiling')
arg_parser.add_argument('--prefix', dest='install_prefix', default='/usr/local', help='Root installation path of Seastar files')
args = arg_parser.parse_args()

def identify_best_dialect(dialects, compiler):
    """Returns the first C++ dialect accepted by the compiler in the sequence,
    assuming the "best" dialects appear first.

    If no dialects are accepted, the result is the last dialect in the sequence
    (we assume that this error will be displayed to the user - during compile
    time - in an informative way).

    """
    for d in dialects:
        if dialect_supported(d, compiler):
            return d

    return d

if args.cpp_dialect == '':
    cpp_dialects = ['gnu++17', 'gnu++1z', 'gnu++14', 'gnu++1y']
    args.cpp_dialect = identify_best_dialect(cpp_dialects, compiler=args.cxx)

def infer_dpdk_machine(user_cflags):
    """Infer the DPDK machine identifier (e.g., 'ivb') from the space-separated
    string of user cflags by scraping the value of `-march` if it is present.

    The default if no architecture is indicated is 'native'.
    """
    arch = 'native'

    # `-march` may be repeated, and we want the last one.
    # strip features, leave only the arch: armv8-a+crc+crypto -> armv8-a
    for flag in user_cflags.split():
        if flag.startswith('-march'):
            arch = flag[7:].split('+')[0]

    MAPPING = {
        'native': 'native',
        'nehalem': 'nhm',
        'westmere': 'wsm',
        'sandybridge': 'snb',
        'ivybridge': 'ivb',
        'armv8-a': 'armv8a',
    }

    return MAPPING.get(arch, 'native')

MODES = seastar_cmake.SUPPORTED_MODES if args.mode == 'all' else [args.mode]

# For convenience.
tr = seastar_cmake.translate_arg

MODE_TO_CMAKE_BUILD_TYPE = {'release' : 'RelWithDebInfo', 'debug' : 'Debug', 'dev' : 'Dev', 'sanitize' : 'Sanitize' }

def configure_mode(mode):
    BUILD_PATH = seastar_cmake.BUILD_PATHS[mode]

    CFLAGS = seastar_cmake.convert_strings_to_cmake_list(
        args.user_cflags,
        args.user_optflags if seastar_cmake.is_release_mode(mode) else '')

    LDFLAGS = seastar_cmake.convert_strings_to_cmake_list(args.user_ldflags)

    TRANSLATED_ARGS = [
        '-DCMAKE_BUILD_TYPE={}'.format(MODE_TO_CMAKE_BUILD_TYPE[mode]),
        '-DCMAKE_C_COMPILER={}'.format(args.cc),
        '-DCMAKE_CXX_COMPILER={}'.format(args.cxx),
        '-DCMAKE_INSTALL_PREFIX={}'.format(args.install_prefix),
        '-DSeastar_API_LEVEL={}'.format(args.api_level),
        tr(args.exclude_tests, 'EXCLUDE_TESTS_FROM_ALL'),
        tr(args.exclude_apps, 'EXCLUDE_APPS_FROM_ALL'),
        tr(args.exclude_demos, 'EXCLUDE_DEMOS_FROM_ALL'),
        tr(CFLAGS, 'CXX_FLAGS'),
        tr(LDFLAGS, 'LD_FLAGS'),
        tr(args.cpp_dialect, 'CXX_DIALECT'),
        tr(args.dpdk, 'DPDK'),
        tr(infer_dpdk_machine(args.user_cflags), 'DPDK_MACHINE'),
        tr(args.hwloc, 'HWLOC', value_when_none='yes'),
        tr(args.alloc_failure_injection, 'ALLOC_FAILURE_INJECTION'),
        tr(args.task_backtrace, 'TASK_BACKTRACE'),
        tr(args.alloc_page_size, 'ALLOC_PAGE_SIZE'),
        tr(args.split_dwarf, 'SPLIT_DWARF'),
        tr(args.heap_profiling, 'HEAP_PROFILING'),
        tr(args.coroutines_ts, 'EXPERIMENTAL_COROUTINES_TS'),
        tr(args.unused_result_error, 'UNUSED_RESULT_ERROR'),
    ]

    ingredients_to_cook = set(args.cook)

    if args.dpdk:
        ingredients_to_cook.add('dpdk')

    # Generate a new build by pointing to the source directory.
    if ingredients_to_cook:
        # We need to use cmake-cooking for some dependencies.
        inclusion_arguments = []

        for ingredient in ingredients_to_cook:
            inclusion_arguments.extend(['-i', ingredient])

        ARGS = seastar_cmake.COOKING_BASIC_ARGS + inclusion_arguments + ['-d', BUILD_PATH, '--']
        dir = seastar_cmake.ROOT_PATH
    else:
        # When building without cooked dependencies, we can invoke cmake directly. We can't call
        # cooking.sh, because without any -i parameters, it will try to build
        # everything.
        ARGS = ['cmake', '-G', 'Ninja', '../..']
        dir = BUILD_PATH
    ARGS += TRANSLATED_ARGS
    if args.verbose:
        print("Running CMake in '{}' ...".format(dir))
        print(" \\\n  ".join(ARGS))
    distutils.dir_util.mkpath(BUILD_PATH)
    subprocess.check_call(ARGS, shell=False, cwd=dir)

for mode in MODES:
    configure_mode(mode)
