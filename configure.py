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
import seastar_cmake
import subprocess
import tempfile

tempfile.tempdir = "./build/tmp"


def add_tristate(arg_parser, name, dest, help, default=None):
    arg_parser.add_argument('--enable-' + name, dest=dest, action='store_true', default=default,
                            help='Enable ' + help + ' [default]' if default else '')
    arg_parser.add_argument('--disable-' + name, dest=dest, action='store_false', default=None,
                            help='Disable ' + help)


def try_compile(compiler, source='', flags=[]):
    return try_compile_and_link(compiler, source, flags=flags + ['-c'])


def ensure_tmp_dir_exists():
    if not os.path.exists(tempfile.tempdir):
        os.makedirs(tempfile.tempdir)


def try_compile_and_link(compiler, source='', flags=[]):
    ensure_tmp_dir_exists()
    with tempfile.NamedTemporaryFile() as sfile:
        ofd, ofile = tempfile.mkstemp()
        os.close(ofd)
        try:
            sfile.file.write(bytes(source, 'utf-8'))
            sfile.file.flush()
            # We can't write to /dev/null, since in some cases (-ftest-coverage) gcc will create an auxiliary
            # output file based on the name of the output file, and "/dev/null.gcsa" is not a good name
            return subprocess.call([compiler, '-x', 'c++', '-o', ofile, sfile.name] + flags,
                                   stdout=subprocess.DEVNULL,
                                   stderr=subprocess.DEVNULL) == 0
        finally:
            if os.path.exists(ofile):
                os.unlink(ofile)


def standard_supported(standard, compiler='g++'):
    return try_compile(compiler=compiler, source='', flags=['-std=' + standard])


arg_parser = argparse.ArgumentParser('Configure seastar')
arg_parser.add_argument('--mode', action='store', choices=seastar_cmake.SUPPORTED_MODES + ['all'], default='all')
arg_parser.add_argument('--build-root', action='store', default=seastar_cmake.DEFAULT_BUILD_ROOT, type=str,
                        help='The name of the build root build directoy: using a different name allows multiple '
                        'configurations to co-exist in the same repository')
arg_parser.add_argument('--cflags', action = 'store', dest='user_cflags', default='',
                        help='Extra flags for the C++ compiler')
arg_parser.add_argument('--ldflags', action='store', dest='user_ldflags', default='',
                        help='Extra flags for the linker')
arg_parser.add_argument('--optflags', action='store', dest='user_optflags', default='',
                        help='Extra optimization flags for the release mode')
arg_parser.add_argument('--api-level', action='store', dest='api_level', default='7',
                        help='Compatibility API level (7=latest)')
arg_parser.add_argument('--compiler', action='store', dest='cxx', default='g++',
                        help='C++ compiler path')
arg_parser.add_argument('--c-compiler', action='store', dest='cc', default='gcc',
                        help='C compiler path (for bundled libraries such as dpdk)')
arg_parser.add_argument('--ccache', nargs='?', const='ccache', default='', metavar='CCACHE_BINARY_PATH',
                        help='Use ccache to cache compilation (and optionally provide a path to ccache binary)')
arg_parser.add_argument('--c++-standard', action='store', dest='cpp_standard', default='',
                        help='C++ standard to build with')
arg_parser.add_argument('--cook', action='append', dest='cook', default=[],
                        help='Supply this dependency locally for development via `cmake-cooking` (can be repeated)')
arg_parser.add_argument('--verbose', dest='verbose', action='store_true', help='Make configure output more verbose.')
arg_parser.add_argument('--scheduling-groups-count', action='store', dest='scheduling_groups_count', default='16',
                        help='Number of available scheduling groups in the reactor')

add_tristate(
    arg_parser,
    name='dpdk',
    dest='dpdk',
    help='DPDK support')
add_tristate(
    arg_parser,
    name='cxx-modules',
    dest='cxx_modules',
    help='build as C++20 module')
add_tristate(
    arg_parser,
    name='hwloc',
    dest='hwloc',
    help='hwloc support')
add_tristate(
    arg_parser,
    name='alloc-failure-injector',
    dest='alloc_failure_injection',
    help='allocation failure injection')
add_tristate(
    arg_parser,
    name='task-backtrace',
    dest='task_backtrace',
    help='Collect backtrace at deferring points')
add_tristate(
    arg_parser,
    name='unused-result-error',
    dest="unused_result_error",
    help='Make [[nodiscard]] violations an error')
add_tristate(
    arg_parser,
    name='debug-shared-ptr',
    dest="debug_shared_ptr",
    help='Debug shared_ptr')
add_tristate(
    arg_parser,
    name='io_uring',
    dest='io_uring',
    help='Support io_uring via liburing')
arg_parser.add_argument('--allocator-page-size', dest='alloc_page_size', type=int, help='override allocator page size')
arg_parser.add_argument('--without-tests', dest='exclude_tests', action='store_true', help='Do not build tests by default')
arg_parser.add_argument('--without-apps', dest='exclude_apps', action='store_true', help='Do not build applications by default')
arg_parser.add_argument('--without-demos', dest='exclude_demos', action='store_true', help='Do not build demonstrations by default')
arg_parser.add_argument('--split-dwarf', dest='split_dwarf', action='store_true', default=False,
                        help='use of split dwarf (https://gcc.gnu.org/wiki/DebugFission) to speed up linking')
arg_parser.add_argument('--compile-commands-json', dest='cc_json', action='store_true',
                        help='Generate a compile_commands.json file for integration with clangd and other tools.')
arg_parser.add_argument('--heap-profiling', dest='heap_profiling', action='store_true', default=False, help='Enable heap profiling')
arg_parser.add_argument('--dpdk-machine', default='native', help='Specify the target architecture')
add_tristate(arg_parser, name='deferred-action-require-noexcept', dest='deferred_action_require_noexcept', help='noexcept requirement for deferred actions', default=True)
arg_parser.add_argument('--prefix', dest='install_prefix', default='/usr/local', help='Root installation path of Seastar files')
args = arg_parser.parse_args()


def identify_best_standard(cpp_standards, compiler):
    """Returns the first C++ standard accepted by the compiler in the sequence,
    assuming the "best" standards appear first.

    If no standards are accepted, we fail configure.py. There is not point
    of letting the user attempt to build with a standard that is known not
    to be supported.
    """
    for std in cpp_standards:
        if standard_supported('c++{}'.format(std), compiler):
            return std
    raise Exception(f"{compiler} does not seem to support any of Seastar's preferred C++ standards - {cpp_standards}. Please upgrade your compiler.")


if not args.cpp_standard:
    cpp_standards = ['23', '20']
    args.cpp_standard = identify_best_standard(cpp_standards, compiler=args.cxx)


MODES = seastar_cmake.SUPPORTED_MODES if args.mode == 'all' else [args.mode]

# For convenience.
tr = seastar_cmake.translate_arg

MODE_TO_CMAKE_BUILD_TYPE = {'release': 'RelWithDebInfo', 'debug': 'Debug', 'dev': 'Dev', 'sanitize': 'Sanitize' }


def configure_mode(mode):
    BUILD_PATH = seastar_cmake.build_path(mode, build_root=args.build_root)

    CFLAGS = seastar_cmake.convert_strings_to_cmake_list(
        args.user_cflags,
        args.user_optflags if seastar_cmake.is_release_mode(mode) else '')

    LDFLAGS = seastar_cmake.convert_strings_to_cmake_list(args.user_ldflags)

    TRANSLATED_ARGS = [
        '-DCMAKE_BUILD_TYPE={}'.format(MODE_TO_CMAKE_BUILD_TYPE[mode]),
        '-DCMAKE_CXX_COMPILER={}'.format(args.cxx),
        '-DCMAKE_CXX_STANDARD={}'.format(args.cpp_standard),
        '-DCMAKE_CXX_COMPILER_LAUNCHER={}'.format(args.ccache),
        '-DCMAKE_INSTALL_PREFIX={}'.format(args.install_prefix),
        '-DCMAKE_EXPORT_COMPILE_COMMANDS={}'.format('yes' if args.cc_json else 'no'),
        '-DBUILD_SHARED_LIBS={}'.format('yes' if mode in ('debug', 'dev') else 'no'),
        '-DSeastar_API_LEVEL={}'.format(args.api_level),
        '-DSeastar_SCHEDULING_GROUPS_COUNT={}'.format(args.scheduling_groups_count),
        tr(args.exclude_tests, 'EXCLUDE_TESTS_FROM_ALL'),
        tr(args.exclude_apps, 'EXCLUDE_APPS_FROM_ALL'),
        tr(args.exclude_demos, 'EXCLUDE_DEMOS_FROM_ALL'),
        tr(CFLAGS, 'CXX_FLAGS'),
        tr(LDFLAGS, 'LD_FLAGS'),
        tr(args.cxx_modules, 'MODULE'),
        tr(args.dpdk, 'DPDK'),
        tr(args.dpdk_machine, 'DPDK_MACHINE'),
        tr(args.hwloc, 'HWLOC', value_when_none='yes'),
        tr(args.io_uring, 'IO_URING', value_when_none=None),
        tr(args.alloc_failure_injection, 'ALLOC_FAILURE_INJECTION', value_when_none='DEFAULT'),
        tr(args.task_backtrace, 'TASK_BACKTRACE'),
        tr(args.alloc_page_size, 'ALLOC_PAGE_SIZE'),
        tr(args.split_dwarf, 'SPLIT_DWARF'),
        tr(args.heap_profiling, 'HEAP_PROFILING'),
        tr(args.deferred_action_require_noexcept, 'DEFERRED_ACTION_REQUIRE_NOEXCEPT'),
        tr(args.unused_result_error, 'UNUSED_RESULT_ERROR'),
        tr(args.debug_shared_ptr, 'DEBUG_SHARED_PTR', value_when_none='default'),
    ]

    ingredients_to_cook = set(args.cook)

    if args.dpdk:
        ingredients_to_cook.add('dpdk')

    # Generate a new build by pointing to the source directory.
    if ingredients_to_cook:
        # the C compiler is only used when building ingredients.
        TRANSLATED_ARGS.append(f'-DCMAKE_C_COMPILER={args.cc}')

        # We need to use cmake-cooking for some dependencies.
        inclusion_arguments = []

        for ingredient in ingredients_to_cook:
            inclusion_arguments.extend(['-i', ingredient])

        ARGS = seastar_cmake.COOKING_BASIC_ARGS + inclusion_arguments
        if args.user_cflags:
            ARGS += ['-s', f'CXXFLAGS={args.user_cflags}']
        if args.user_ldflags:
            ARGS += ['-s', f'LDFLAGS={args.user_ldflags}']
        ARGS += ['-d', BUILD_PATH, '--']
        dir = seastar_cmake.ROOT_PATH
    else:
        # When building without cooked dependencies, we can invoke cmake directly. We can't call
        # cooking.sh, because without any -i parameters, it will try to build
        # everything.
        root_relative_to_build = os.path.relpath(seastar_cmake.ROOT_PATH, BUILD_PATH)
        ARGS = ['cmake', '-G', 'Ninja', root_relative_to_build]
        dir = BUILD_PATH
    # filter out empty args, their values are actually "guess",
    # CMake should be able to figure it out.
    ARGS += filter(lambda arg: arg, TRANSLATED_ARGS)
    if args.verbose:
        print("Running CMake in '{}' ...".format(dir))
        print(" \\\n  ".join(ARGS))
    os.makedirs(BUILD_PATH, exist_ok=True)
    subprocess.check_call(ARGS, shell=False, cwd=dir)


for mode in MODES:
    configure_mode(mode)
