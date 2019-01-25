#!/usr/bin/python3
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
arg_parser.add_argument('--compiler', action = 'store', dest = 'cxx', default = 'g++',
                        help = 'C++ compiler path')
arg_parser.add_argument('--c-compiler', action='store', dest='cc', default='gcc',
                        help = 'C compiler path (for bundled libraries such as dpdk)')
arg_parser.add_argument('--c++-dialect', action='store', dest='cpp_dialect', default='',
                        help='C++ dialect to build with [default: %(default)s]')
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
    name = 'gcc6-concepts',
    dest = 'gcc6_concepts',
    help = 'experimental support for C++ Concepts as implemented in GCC 6')
add_tristate(
    arg_parser,
    name = 'alloc-failure-injector',
    dest = 'alloc_failure_injector',
    help = 'allocation failure injection')
add_tristate(
    arg_parser,
    name = 'exception-scalability-workaround',
    dest = 'exception_workaround',
    help = 'a workaround for C++ exception scalability issues by overriding the definition of `dl_iterate_phdr`')
arg_parser.add_argument('--allocator-page-size', dest='allocator_page_size', type=int, help='override allocator page size')
arg_parser.add_argument('--without-tests', dest='exclude_tests', action='store_true', help='Do not build tests by default')
arg_parser.add_argument('--without-apps', dest='exclude_apps', action='store_true', help='Do not build applications by default')
arg_parser.add_argument('--without-demos', dest='exclude_demos', action='store_true', help='Do not build demonstrations by default')
arg_parser.add_argument('--use-std-optional-variant-stringview', dest='cpp17_goodies', action='store', type=int, default=0,
                        help='Use C++17 std types for optional, variant, and string_view. Requires C++17 dialect and GCC >= 8.1.1-5')
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

MODES = seastar_cmake.SUPPORTED_MODES if args.mode == 'all' else [args.mode]

# For convenience.
tr = seastar_cmake.translate_arg

MODE_TO_CMAKE_BUILD_TYPE = {'release' : 'RelWithDebInfo', 'debug' : 'Debug', 'dev' : 'Dev' }

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
        tr(args.exclude_tests, 'EXCLUDE_TESTS_FROM_ALL'),
        tr(args.exclude_apps, 'EXCLUDE_APPS_FROM_ALL'),
        tr(args.exclude_demos, 'EXCLUDE_DEMOS_FROM_ALL'),
        tr(CFLAGS, 'CXX_FLAGS'),
        tr(LDFLAGS, 'LD_FLAGS'),
        tr(args.cpp_dialect, 'CXX_DIALECT'),
        tr(args.dpdk, 'DPDK'),
        tr(args.hwloc, 'HWLOC', value_when_none='yes'),
        tr(args.gcc6_concepts, 'GCC6_CONCEPTS'),
        tr(args.alloc_failure_injector, 'ALLOC_FAILURE_INJECTOR'),
        tr(args.exception_workaround, 'EXCEPTION_SCALABILITY_WORKAROUND', value_when_none='yes'),
        tr(args.allocator_page_size, 'ALLOCATOR_PAGE_SIZE'),
        tr(args.cpp17_goodies, 'STD_OPTIONAL_VARIANT_STRINGVIEW'),
    ]

    # Generate a new build by pointing to the source directory.
    ARGS = seastar_cmake.COOKING_BASIC_ARGS + (['-i', 'dpdk'] if args.dpdk else []) + ['-d', BUILD_PATH, '--'] + TRANSLATED_ARGS
    print(ARGS)
    distutils.dir_util.mkpath(BUILD_PATH)
    subprocess.check_call(ARGS, shell=False, cwd=seastar_cmake.ROOT_PATH)

for mode in MODES:
    configure_mode(mode)
