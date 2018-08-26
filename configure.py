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
import os, os.path, textwrap, argparse, sys, shlex, subprocess, tempfile, re
import distutils.dir_util
import distutils.spawn
import seastar_cmake

configure_args = str.join(' ', [shlex.quote(x) for x in sys.argv[1:]])

tempfile.tempdir = "./build/tmp"

srcdir = os.getcwd()

ninja_exe = distutils.spawn.find_executable('ninja-build') or distutils.spawn.find_executable('ninja')

def get_flags():
    with open('/proc/cpuinfo') as f:
        for line in f:
            if line.strip():
                if line.rstrip('\n').startswith('flags'):
                    return re.sub(r'^flags\s+: ', '', line).split()

def add_tristate(arg_parser, name, dest, help):
    arg_parser.add_argument('--enable-' + name, dest = dest, action = 'store_true', default = None,
                            help = 'Enable ' + help)
    arg_parser.add_argument('--disable-' + name, dest = dest, action = 'store_false', default = None,
                            help = 'Disable ' + help)

def apply_tristate(var, test, note, missing):
    if (var is None) or var:
        if test():
            return True
        elif var == True:
            print(missing)
            sys.exit(1)
        else:
            print(note)
            return False
    return False

#
# dpdk_cflags - fetch the DPDK specific CFLAGS
#
# Run a simple makefile that "includes" the DPDK main makefile and prints the
# MACHINE_CFLAGS value
#
def dpdk_cflags (dpdk_target):
    ensure_tmp_dir_exists()
    with tempfile.NamedTemporaryFile() as sfile:
        dpdk_target = os.path.abspath(dpdk_target)
        dpdk_target = re.sub(r'\/+$', '', dpdk_target)
        dpdk_sdk_path = os.path.dirname(dpdk_target)
        dpdk_target_name = os.path.basename(dpdk_target)
        dpdk_arch = dpdk_target_name.split('-')[0]
        if args.dpdk:
            dpdk_sdk_path = 'dpdk'
            dpdk_target = os.getcwd() + '/build/dpdk'
            dpdk_target_name = 'x86_64-{}-linuxapp-gcc'.format(dpdk_machine)
            dpdk_arch = 'x86_64'

        sfile.file.write(bytes('include ' + dpdk_sdk_path + '/mk/rte.vars.mk' + "\n", 'utf-8'))
        sfile.file.write(bytes('all:' + "\n\t", 'utf-8'))
        sfile.file.write(bytes('@echo $(MACHINE_CFLAGS)' + "\n", 'utf-8'))
        sfile.file.flush()

        dpdk_cflags = subprocess.check_output(['make', '--no-print-directory',
                                             '-f', sfile.name,
                                             'RTE_SDK=' + dpdk_sdk_path,
                                             'RTE_OUTPUT=' + dpdk_target,
                                             'RTE_TARGET=' + dpdk_target_name,
                                             'RTE_SDK_BIN=' + dpdk_target,
                                             'RTE_ARCH=' + dpdk_arch])
        dpdk_cflags_str = dpdk_cflags.decode('utf-8')
        dpdk_cflags_str = re.sub(r'\n+$', '', dpdk_cflags_str)
        dpdk_cflags_final = ''

        return dpdk_cflags_str

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

def try_compile_and_run(compiler, flags, source, env = {}):
    ensure_tmp_dir_exists()
    mktemp = tempfile.NamedTemporaryFile
    with mktemp() as sfile, mktemp(mode='rb') as xfile:
        sfile.file.write(bytes(source, 'utf-8'))
        sfile.file.flush()
        xfile.file.close()
        if subprocess.call([compiler, '-x', 'c++', '-o', xfile.name, sfile.name] + args.user_cflags.split() + flags,
                            stdout = subprocess.DEVNULL, stderr = subprocess.DEVNULL) != 0:
            # The compiler may delete the target on failure, and lead to
            # NamedTemporaryFile's destructor throwing an exception.
            open(xfile.name, 'a').close()
            return False
        e = os.environ.copy()
        e.update(env)
        env = e
        return subprocess.call([xfile.name], stdout = subprocess.DEVNULL, stderr = subprocess.DEVNULL, env=env) == 0

def warning_supported(warning, compiler, flags):
    # gcc ignores -Wno-x even if it is not supported
    adjusted = re.sub('^-Wno-', '-W', warning)
    return try_compile(flags=flags + [adjusted, '-Werror'], compiler = compiler)

def debug_flag(compiler, flags):
    src_with_auto = textwrap.dedent('''\
        template <typename T>
        struct x { auto f() {} };

        x<int> a;
        ''')
    if try_compile(source = src_with_auto, flags = flags + ['-g', '-std=gnu++1y'], compiler = compiler):
        return '-g'
    else:
        print('Note: debug information disabled; upgrade your compiler')
        return ''

def dialect_supported(dialect, compiler='g++'):
    return try_compile(compiler=compiler, source='', flags=['-std=' + dialect])

def detect_membarrier(compiler, flags):
    return try_compile(compiler=compiler, flags=flags, source=textwrap.dedent('''\
        #include <linux/membarrier.h>
        
        int x = MEMBARRIER_CMD_PRIVATE_EXPEDITED | MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED;
        '''))

def sanitize_vptr_flag(compiler, flags):
    # https://gcc.gnu.org/bugzilla/show_bug.cgi?id=67258
    if (not try_compile(compiler, flags=flags + ['-fsanitize=vptr'])
        or (try_compile_and_run(compiler, flags=flags + ['-fsanitize=undefined', '-fno-sanitize-recover'],
                               env={'UBSAN_OPTIONS': 'exitcode=1'}, source=textwrap.dedent('''
            struct A
            {
                virtual ~A() {}
            };
            struct B : virtual A {};
            struct C : virtual A {};
            struct D : B, virtual C {};

            int main()
            {
                D d;
            }
            '''))
            and False)):   # -fsanitize=vptr is broken even when the test above passes
        return ''
    else:
        print('Notice: -fsanitize=vptr is broken, disabling; some debug mode tests are bypassed.')
        return '-fno-sanitize=vptr'


def adjust_visibility_flags(compiler, flags):
    # https://gcc.gnu.org/bugzilla/show_bug.cgi?id=80947
    flags = flags + ['-fvisibility=hidden', '-std=gnu++1y', '-Werror=attributes']
    if not try_compile(compiler, flags=flags, source=textwrap.dedent('''
            template <class T>
            class MyClass  {
            public:
                MyClass() {
                    auto outer = [this] ()
                        {
                            auto fn = [this]   {  };
                            //use fn for something here
                        };
                }
            };

            template<typename T>
            void foo() {
                struct inner {
                    inner() {
                        (void)([this] { });
                    }
                };
            }

            int main() {
                 MyClass<int> r;
                 foo<int>();
            }
            ''')):
        print('Notice: disabling -Wattributes due to https://gcc.gnu.org/bugzilla/show_bug.cgi?id=80947')
        return '-Wno-attributes'
    else:
        return ''

def configure_fmt(mode, cxx='g++', cc='gcc'):
    builddir = 'build/{}/fmt'.format(mode)
    os.makedirs(builddir, exist_ok=True)
    subprocess.check_output(args=['cmake', '-G', 'Ninja', '../../../fmt', '-DCMAKE_CXX_COMPILER=' + cxx, '-DCMAKE_C_COMPILER=' + cc], cwd=builddir)

modes = {
    'debug': {
        'sanitize': '-fsanitize=address -fsanitize=leak -fsanitize=undefined',
        'sanitize_libs': '-lasan -lubsan',
        'opt': '-O0 -DSEASTAR_DEBUG -DSEASTAR_DEBUG_SHARED_PTR -DSEASTAR_DEFAULT_ALLOCATOR -DSEASTAR_THREAD_STACK_GUARDS -DSEASTAR_NO_EXCEPTION_HACK -DSEASTAR_SHUFFLE_TASK_QUEUE',
        'libs': '',
        'cares_opts': '-DCARES_STATIC=ON -DCARES_SHARED=OFF -DCMAKE_BUILD_TYPE=Debug',
    },
    'release': {
        'sanitize': '',
        'sanitize_libs': '',
        'opt': '-O2',
        'libs': '',
        'cares_opts': '-DCARES_STATIC=ON -DCARES_SHARED=OFF -DCMAKE_BUILD_TYPE=Release',
    },
}

perf_tests = [
    'tests/perf/perf_future_util',
]

tests = [
    'tests/file_io_test',
    'tests/directory_test',
    'tests/linecount',
    'tests/echotest',
    'tests/l3_test',
    'tests/ip_test',
    'tests/timer_test',
    'tests/tcp_test',
    'tests/futures_test',
    'tests/alloc_test',
    'tests/foreign_ptr_test',
    'tests/smp_test',
    'tests/thread_test',
    'tests/thread_context_switch_test',
    'tests/udp_server',
    'tests/udp_client',
    'tests/blkdiscard_test',
    'tests/sstring_test',
    'tests/unwind_test',
    'tests/defer_test',
    'tests/httpd_test',
    'tests/memcached/memcached_ascii_parser_test',
    'tests/tcp_sctp_server',
    'tests/tcp_sctp_client',
    'tests/allocator_test',
    'tests/output_stream_test',
    'tests/udp_zero_copy',
    'tests/shared_ptr_test',
    'tests/weak_ptr_test',
    'tests/checked_ptr_test',
    'tests/slab_test',
    'tests/fstream_test',
    'tests/distributed_test',
    'tests/rpc',
    'tests/semaphore_test',
    'tests/expiring_fifo_test',
    'tests/packet_test',
    'tests/tls_test',
    'tests/fair_queue_test',
    'tests/rpc_test',
    'tests/connect_test',
    'tests/chunked_fifo_test',
    'tests/circular_buffer_test',
    'tests/perf/perf_fstream',
    'tests/json_formatter_test',
    'tests/dns_test',
    'tests/execution_stage_test',
    'tests/lowres_clock_test',
    'tests/program_options_test',
    'tests/tuple_utils_test',
    'tests/tls_echo_server',
    'tests/tls_simple_client',
    'tests/circular_buffer_fixed_capacity_test',
    'tests/noncopyable_function_test',
    'tests/netconfig_test',
    'tests/abort_source_test',
    'tests/alien_test',
    'tests/signal_test',
    ] + perf_tests

apps = [
    'apps/httpd/httpd',
    'apps/seawreck/seawreck',
    'apps/io_tester/io_tester',
    'apps/memcached/memcached',
    'apps/iotune/iotune',
    'tests/scheduling_group_demo',
    ]

extralibs = {
    'apps/io_tester/io_tester': [ '-lyaml-cpp' ]
}

all_artifacts = apps + tests + ['libseastar.a', 'seastar.pc', 'fmt/fmt/libfmt.a']

arg_parser = argparse.ArgumentParser('Configure seastar')
arg_parser.add_argument('--static', dest = 'static', action = 'store_const', default = '',
                        const = '-static',
                        help = 'Static link (useful for running on hosts outside the build environment')
arg_parser.add_argument('--pie', dest = 'pie', action = 'store_true',
                        help = 'Build position-independent executable (PIE)')
arg_parser.add_argument('--so', dest = 'so', action = 'store_true',
                        help = 'Build shared object (SO) instead of executable')
arg_parser.add_argument('--mode', action='store', choices=list(modes.keys()) + ['all'], default='all')
arg_parser.add_argument('--with', dest='artifacts', action='append', choices=all_artifacts, default=[])
arg_parser.add_argument('--cflags', action = 'store', dest = 'user_cflags', default = '',
                        help = 'Extra flags for the C++ compiler')
arg_parser.add_argument('--ldflags', action = 'store', dest = 'user_ldflags', default = '',
                        help = 'Extra flags for the linker')
arg_parser.add_argument('--optflags', action = 'store', dest = 'user_optflags', default = '',
                        help = 'Extra optimization flags for the release mode')
arg_parser.add_argument('--compiler', action = 'store', dest = 'cxx', default = 'g++',
                        help = 'C++ compiler path')
arg_parser.add_argument('--c-compiler', action='store', dest='cc', default='gcc',
                        help = 'C compiler path (for bundled libraries such as dpdk and c-ares)')
arg_parser.add_argument('--c++-dialect', action='store', dest='cpp_dialect', default='',
                        help='C++ dialect to build with [default: %(default)s]')
arg_parser.add_argument('--with-osv', action = 'store', dest = 'with_osv', default = '',
                        help = 'Shortcut for compile for OSv')
arg_parser.add_argument('--enable-dpdk', action = 'store_true', dest = 'dpdk', default = False,
                        help = 'Enable dpdk (from included dpdk sources)')
arg_parser.add_argument('--dpdk-target', action = 'store', dest = 'dpdk_target', default = '',
                        help = 'Path to DPDK SDK target location (e.g. <DPDK SDK dir>/x86_64-native-linuxapp-gcc)')
arg_parser.add_argument('--debuginfo', action = 'store', dest = 'debuginfo', type = int, default = 1,
                        help = 'Enable(1)/disable(0)compiler debug information generation')
arg_parser.add_argument('--tests-debuginfo', action='store', dest='tests_debuginfo', type=int, default=0,
                        help='Enable(1)/disable(0)compiler debug information generation for tests')
arg_parser.add_argument('--static-stdc++', dest = 'staticcxx', action = 'store_true',
                        help = 'Link libgcc and libstdc++ statically')
arg_parser.add_argument('--static-boost', dest = 'staticboost', action = 'store_true',
                        help = 'Link with boost statically')
arg_parser.add_argument('--static-yaml-cpp', dest = 'staticyamlcpp', action = 'store_true',
            help = 'Link libyaml-cpp statically')
add_tristate(arg_parser, name = 'hwloc', dest = 'hwloc', help = 'hwloc support')
arg_parser.add_argument('--enable-gcc6-concepts', dest='gcc6_concepts', action='store_true', default=False,
                        help='enable experimental support for C++ Concepts as implemented in GCC 6')
arg_parser.add_argument('--enable-alloc-failure-injector', dest='alloc_failure_injector', action='store_true', default=False,
                        help='enable allocation failure injection')
add_tristate(arg_parser, name = 'exception-scalability-workaround', dest='exception_workaround',
        help='disabling override of dl_iterate_phdr symbol to workaround C++ exception scalability issues')
arg_parser.add_argument('--allocator-page-size', dest='allocator_page_size', type=int, help='override allocator page size')
arg_parser.add_argument('--protoc-compiler', action = 'store', dest='protoc', default='protoc',
                        help = 'Path to protoc compiler, the default is protoc')
arg_parser.add_argument('--cmake', dest='cmake', action='store_true',
                        help='Use CMake as the underlying build-sytem')
arg_parser.add_argument('--without-tests', dest='exclude_tests', action='store_true', help='Do not build tests by default (CMake only)')
arg_parser.add_argument('--without-apps', dest='exclude_apps', action='store_true', help='Do not build applications by default (CMake only)')
arg_parser.add_argument('--use-std-optional-variant-stringview', dest='cpp17_goodies', action='store', type=int, default=0,
                        help='Use C++17 std types for optional, variant, and string_view. Requires C++17 dialect and GCC >= 8.1.1-5')
args = arg_parser.parse_args()


if args.cpp_dialect == '':
    cpp_dialects = ['gnu++17', 'gnu++1z', 'gnu++14', 'gnu++1y']
    try:
        args.cpp_dialect = [x for x in cpp_dialects if dialect_supported(x, compiler=args.cxx)][0]
    except:
        # if g++ is not available, fallback to something safe-ish
        args.cpp_dialect='gnu++1y'

# Forwarding to CMake.
if args.cmake:
    MODES = seastar_cmake.SUPPORTED_MODES if args.mode is 'all' else [args.mode]

    # For convenience.
    tr = seastar_cmake.translate_arg

    def configure_mode(mode):
        BUILD_PATH = seastar_cmake.BUILD_PATHS[mode]

        TRANSLATED_ARGS = [
            '-DCMAKE_BUILD_TYPE={}'.format(mode.title()),
            '-DCMAKE_C_COMPILER={}'.format(args.cc),
            '-DCMAKE_CXX_COMPILER={}'.format(args.cxx),
            tr(args.exclude_tests, 'EXCLUDE_TESTS_BY_DEFAULT'),
            tr(args.exclude_apps, 'EXCLUDE_APPS_BY_DEFAULT'),
            tr(args.user_cflags, 'USER_CXXFLAGS'),
            tr(args.user_ldflags, 'USER_LDFLAGS'),
            tr(args.user_optflags, 'CXX_OPTIMIZATION_FLAGS'),
            tr(args.cpp_dialect, 'CXX_DIALECT'),
            tr(args.dpdk, 'ENABLE_DPDK'),
            tr(args.staticboost, 'LINK_STATIC_BOOST'),
            tr(args.staticyamlcpp, 'LINK_STATIC_YAML_CPP'),
            tr(args.hwloc, 'ENABLE_HWLOC'),
            tr(args.gcc6_concepts, 'ENABLE_GCC6_CONCEPTS'),
            tr(args.alloc_failure_injector, 'ENABLE_ALLOC_FAILURE_INJECTOR'),
            tr(args.exception_workaround, 'ENABLE_EXCEPTION_SCALABILITY_WORKAROUND'),
            tr(args.allocator_page_size, 'ALLOCATOR_PAGE_SIZE'),
            tr(args.cpp17_goodies, 'USE_STD_OPTIONAL_VARIANT_STRINGVIEW'),
        ]

        # Generate a new build by pointing to the source directory.
        ARGS = seastar_cmake.CMAKE_BASIC_ARGS + [seastar_cmake.ROOT_PATH] + TRANSLATED_ARGS
        print(ARGS)
        distutils.dir_util.mkpath(BUILD_PATH)
        subprocess.check_call(ARGS, shell=False, cwd=BUILD_PATH)

    for mode in MODES:
        configure_mode(mode)

    sys.exit(0)

libnet = [
    'net/proxy.cc',
    'net/virtio.cc',
    'net/dpdk.cc',
    'net/ip.cc',
    'net/ethernet.cc',
    'net/arp.cc',
    'net/native-stack.cc',
    'net/ip_checksum.cc',
    'net/udp.cc',
    'net/tcp.cc',
    'net/dhcp.cc',
    'net/tls.cc',
    'net/dns.cc',
    'net/config.cc',
    ]

core = [
    'core/reactor.cc',
    'core/alien.cc',
    'core/execution_stage.cc',
    'core/systemwide_memory_barrier.cc',
    'core/fstream.cc',
    'core/posix.cc',
    'core/memory.cc',
    'core/resource.cc',
    'core/scollectd.cc',
    'core/metrics.cc',
    'core/app-template.cc',
    'core/thread.cc',
    'core/dpdk_rte.cc',
    'core/fsqual.cc',
    'core/linux-aio.cc',
    'util/conversions.cc',
    'util/program-options.cc',
    'util/log.cc',
    'util/backtrace.cc',
    'util/alloc_failure_injector.cc',
    'net/packet.cc',
    'net/posix-stack.cc',
    'net/net.cc',
    'net/stack.cc',
    'net/inet_address.cc',
    'rpc/rpc.cc',
    'rpc/lz4_compressor.cc',
    'core/exception_hacks.cc',
    'core/future-util.cc',
    ]

protobuf = [
    'proto/metrics2.proto',
    ]

prometheus = [
    'core/prometheus.cc',
    ]

http = ['http/transformers.cc',
        'http/json_path.cc',
        'http/file_handler.cc',
        'http/common.cc',
        'http/routes.cc',
        'json/json_elements.cc',
        'json/formatter.cc',
        'http/matcher.cc',
        'http/mime_types.cc',
        'http/httpd.cc',
        'http/reply.cc',
        'http/request_parser.rl',
        'http/api_docs.cc',
        ]

boost_test_lib = [
   'tests/test-utils.cc',
   'tests/test_runner.cc',
]


def maybe_static(flag, libs):
    if flag and not args.static:
        libs = '-Wl,-Bstatic {} -Wl,-Bdynamic'.format(libs)
    return libs

defines = []
libs = ' '.join([maybe_static(args.staticboost,
                              '-lboost_program_options -lboost_system -lboost_filesystem'),
                 '-lstdc++ -lm', '-lstdc++fs',
                 maybe_static(args.staticboost, '-lboost_thread'),
                 '-lcryptopp -lrt -lgnutls -lgnutlsxx -llz4 -lprotobuf -ldl -lgcc_s ',
                 maybe_static(args.staticyamlcpp, '-lyaml-cpp'),
                 ])

boost_unit_test_lib = maybe_static(args.staticboost, '-lboost_unit_test_framework')

if args.cpp17_goodies is 1 and args.cpp_dialect == 'gnu++17':
    defines.append('SEASTAR_USE_STD_OPTIONAL_VARIANT_STRINGVIEW')

hwloc_libs = '-lhwloc -lnuma -lpciaccess -lxml2 -lz'

if args.gcc6_concepts or try_compile(args.cxx, source="""#if __cpp_concepts == 201507
int main() { return 0; }
#endif""", flags=['-fconcepts']):
    defines.append('SEASTAR_HAVE_GCC6_CONCEPTS')
    args.user_cflags += ' -fconcepts'

if args.alloc_failure_injector:
    defines.append('SEASTAR_ENABLE_ALLOC_FAILURE_INJECTION')

if not apply_tristate(args.exception_workaround, test = lambda: not args.staticcxx and not args.static,
        note = "Note: disabling exception scalability workaround due to static linkage of libgcc and libstdc++",
        missing = "Error: cannot enable exception scalability workaround with static linkage of libgcc and libstdc++"):
    defines.append('SEASTAR_NO_EXCEPTION_HACK')

if args.staticcxx:
    libs = libs.replace('-lstdc++ ', ' ')
    libs += ' -static-libgcc -static-libstdc++'

if args.staticcxx or args.static:
    defines.append("NO_EXCEPTION_INTERCEPT");

memcache_base = [
    'apps/memcached/ascii.rl'
] + libnet + core

deps = {
    'libseastar.a' : core + libnet + http + protobuf + prometheus,
    'seastar.pc': [],
    'fmt/fmt/libfmt.a': [],
    'apps/httpd/httpd': ['apps/httpd/demo.json', 'apps/httpd/main.cc'] + http + libnet + core,
    'apps/memcached/memcached': ['apps/memcached/memcache.cc'] + memcache_base,
    'tests/memcached/memcached_ascii_parser_test': ['tests/memcached/test_ascii_parser.cc'] + memcache_base,
    'tests/file_io_test': ['tests/fileiotest.cc'] + core,
    'tests/directory_test': ['tests/directory_test.cc'] + core,
    'tests/linecount': ['tests/linecount.cc'] + core,
    'tests/echotest': ['tests/echotest.cc'] + core + libnet,
    'tests/l3_test': ['tests/l3_test.cc'] + core + libnet,
    'tests/ip_test': ['tests/ip_test.cc'] + core + libnet,
    'tests/tcp_test': ['tests/tcp_test.cc'] + core + libnet,
    'tests/timer_test': ['tests/timertest.cc'] + core,
    'tests/futures_test': ['tests/futures_test.cc'] + core,
    'tests/alloc_test': ['tests/alloc_test.cc'] + core,
    'tests/foreign_ptr_test': ['tests/foreign_ptr_test.cc'] + core,
    'tests/semaphore_test': ['tests/semaphore_test.cc'] + core,
    'tests/expiring_fifo_test': ['tests/expiring_fifo_test.cc'] + core,
    'tests/smp_test': ['tests/smp_test.cc'] + core,
    'tests/thread_test': ['tests/thread_test.cc'] + core,
    'tests/thread_context_switch_test': ['tests/thread_context_switch.cc'] + core,
    'tests/udp_server': ['tests/udp_server.cc'] + core + libnet,
    'tests/udp_client': ['tests/udp_client.cc'] + core + libnet,
    'tests/tcp_sctp_server': ['tests/tcp_sctp_server.cc'] + core + libnet,
    'tests/tcp_sctp_client': ['tests/tcp_sctp_client.cc'] + core + libnet,
    'tests/tls_test': ['tests/tls_test.cc'] + core + libnet,
    'tests/fair_queue_test': ['tests/fair_queue_test.cc'] + core,
    'apps/seawreck/seawreck': ['apps/seawreck/seawreck.cc', 'http/http_response_parser.rl'] + core + libnet,
    'apps/io_tester/io_tester': ['apps/io_tester/io_tester.cc'] + core,
    'apps/iotune/iotune': ['apps/iotune/iotune.cc'] + core,
    'tests/blkdiscard_test': ['tests/blkdiscard_test.cc'] + core,
    'tests/sstring_test': ['tests/sstring_test.cc'] + core,
    'tests/unwind_test': ['tests/unwind_test.cc'] + core,
    'tests/defer_test': ['tests/defer_test.cc'] + core,
    'tests/httpd_test': ['tests/httpd.cc'] + http + core,
    'tests/allocator_test': ['tests/allocator_test.cc'] + core,
    'tests/output_stream_test': ['tests/output_stream_test.cc'] + core + libnet,
    'tests/udp_zero_copy': ['tests/udp_zero_copy.cc'] + core + libnet,
    'tests/shared_ptr_test': ['tests/shared_ptr_test.cc'] + core,
    'tests/weak_ptr_test': ['tests/weak_ptr_test.cc'] + core,
    'tests/checked_ptr_test': ['tests/checked_ptr_test.cc'] + core,
    'tests/slab_test': ['tests/slab_test.cc'] + core,
    'tests/fstream_test': ['tests/fstream_test.cc'] + core,
    'tests/distributed_test': ['tests/distributed_test.cc'] + core,
    'tests/rpc': ['tests/rpc.cc'] + core + libnet,
    'tests/rpc_test': ['tests/rpc_test.cc'] + core + libnet,
    'tests/packet_test': ['tests/packet_test.cc'] + core + libnet,
    'tests/connect_test': ['tests/connect_test.cc'] + core + libnet,
    'tests/chunked_fifo_test': ['tests/chunked_fifo_test.cc'] + core,
    'tests/circular_buffer_test': ['tests/circular_buffer_test.cc'] + core,
    'tests/perf/perf_fstream': ['tests/perf/perf_fstream.cc'] + core,
    'tests/json_formatter_test': ['tests/json_formatter_test.cc'] + core + http,
    'tests/dns_test': ['tests/dns_test.cc'] + core + libnet,
    'tests/execution_stage_test': ['tests/execution_stage_test.cc'] + core,
    'tests/lowres_clock_test': ['tests/lowres_clock_test.cc'] + core,
    'tests/program_options_test': ['tests/program_options_test.cc'] + core,
    'tests/tuple_utils_test': ['tests/tuple_utils_test.cc'],
    'tests/tls_echo_server': ['tests/tls_echo_server.cc'] + core + libnet,
    'tests/tls_simple_client': ['tests/tls_simple_client.cc'] + core + libnet,
    'tests/circular_buffer_fixed_capacity_test': ['tests/circular_buffer_fixed_capacity_test.cc'],
    'tests/scheduling_group_demo': ['tests/scheduling_group_demo.cc'] + core,
    'tests/noncopyable_function_test': ['tests/noncopyable_function_test.cc'],
    'tests/netconfig_test': ['tests/netconfig_test.cc'] + core + libnet,
    'tests/abort_source_test': ['tests/abort_source_test.cc'] + core,
    'tests/alien_test': ['tests/alien_test.cc'] + core,
    'tests/signal_test': ['tests/signal_test.cc'] + core,
}

boost_tests = [
    'tests/memcached/memcached_ascii_parser_test',
    'tests/file_io_test',
    'tests/futures_test',
    'tests/alloc_test',
    'tests/foreign_ptr_test',
    'tests/semaphore_test',
    'tests/expiring_fifo_test',
    'tests/thread_test',
    'tests/tls_test',
    'tests/fair_queue_test',
    'tests/httpd_test',
    'tests/output_stream_test',
    'tests/fstream_test',
    'tests/rpc_test',
    'tests/connect_test',
    'tests/json_formatter_test',
    'tests/dns_test',
    'tests/execution_stage_test',
    'tests/lowres_clock_test',
    'tests/abort_source_test',
    'tests/signal_test',
    ]

for bt in boost_tests:
    deps[bt] += boost_test_lib

for pt in perf_tests:
    deps[pt] = [pt + '.cc'] + core + ['tests/perf/perf_tests.cc']

warnings = [
    '-Wno-mismatched-tags',                 # clang-only
    '-Wno-pessimizing-move',                # clang-only: moving a temporary object prevents copy elision
    '-Wno-redundant-move',                  # clang-only: redundant move in return statement
    '-Wno-ignored-attributes',              # clang-only: clang does not support [[gnu::code]]unlikely, but GCC does
    '-Wno-inconsistent-missing-override',   # clang-only: 'x' overrides a member function but is not marked 'override'
    '-Wno-unused-private-field',            # clang-only: private field 'x' is not used
    '-Wno-unknown-attributes',              # clang-only: unknown attribute 'x' ignored (x in this case is gnu::externally_visible)
    '-Wno-unneeded-internal-declaration',   # clang-only: 'x' function 'x' declared in header file shouldb e declared 'x'
    '-Wno-undefined-inline',                # clang-only: inline function 'x' is not defined
    '-Wno-overloaded-virtual',              # clang-only: 'x' hides overloaded virtual functions
    '-Wno-maybe-uninitialized',
    '-Wno-error=cpp',                       # Allow preprocessor warnings
    '-Wno-stringop-overflow',               # gcc: overzealous, false positives
    ]

# The "--with-osv=<path>" parameter is a shortcut for a bunch of other
# settings:
if args.with_osv:
    args.so = True
    args.hwloc = False
    args.user_cflags = (args.user_cflags +
        ' -DSEASTAR_DEFAULT_ALLOCATOR -fvisibility=default -DHAVE_OSV -I' +
        args.with_osv + ' -I' + args.with_osv + '/include -I' +
        args.with_osv + '/arch/x64')

if args.allocator_page_size:
    args.user_cflags += ' -DSEASTAR_OVERRIDE_ALLOCATOR_PAGE_SIZE=' + str(args.allocator_page_size)

dpdk_arch_xlat = {
    'native': 'native',
    'nehalem': 'nhm',
    'westmere': 'wsm',
    'sandybridge': 'snb',
    'ivybridge': 'ivb',
    }

dpdk_machine = 'native'

if args.dpdk:
    if not os.path.exists('dpdk') or not os.listdir('dpdk'):
        raise Exception('--enable-dpdk: dpdk/ is empty. Run "git submodule update --init".')
    cflags = args.user_cflags.split()
    dpdk_machine = ([dpdk_arch_xlat[cflag[7:]]
                     for cflag in cflags
                     if cflag.startswith('-march')] or ['native'])[0]
    subprocess.check_call('make -C dpdk RTE_OUTPUT=$PWD/build/dpdk/ config T=x86_64-native-linuxapp-gcc'.format(
                                                dpdk_machine=dpdk_machine),
                          shell = True)
    # adjust configutation to taste
    dotconfig = 'build/dpdk/.config'
    lines = open(dotconfig, encoding='UTF-8').readlines()
    def update(lines, vars):
        ret = []
        for line in lines:
            for var, val in vars.items():
                if line.startswith(var + '='):
                    line = var + '=' + val + '\n'
            ret.append(line)
        return ret
    lines = update(lines, {'CONFIG_RTE_LIBRTE_PMD_BOND': 'n',
                           'CONFIG_RTE_MBUF_SCATTER_GATHER': 'n',
                           'CONFIG_RTE_LIBRTE_IP_FRAG': 'n',
                           'CONFIG_RTE_APP_TEST': 'n',
                           'CONFIG_RTE_TEST_PMD': 'n',
                           'CONFIG_RTE_MBUF_REFCNT_ATOMIC': 'n',
                           'CONFIG_RTE_MAX_MEMSEG': '8192',
                           'CONFIG_RTE_EAL_IGB_UIO': 'n',
                           'CONFIG_RTE_LIBRTE_KNI': 'n',
                           'CONFIG_RTE_KNI_KMOD': 'n',
                           'CONFIG_RTE_LIBRTE_JOBSTATS': 'n',
                           'CONFIG_RTE_LIBRTE_LPM': 'n',
                           'CONFIG_RTE_LIBRTE_ACL': 'n',
                           'CONFIG_RTE_LIBRTE_POWER': 'n',
                           'CONFIG_RTE_LIBRTE_IP_FRAG': 'n',
                           'CONFIG_RTE_LIBRTE_METER': 'n',
                           'CONFIG_RTE_LIBRTE_SCHED': 'n',
                           'CONFIG_RTE_LIBRTE_DISTRIBUTOR': 'n',
                           'CONFIG_RTE_LIBRTE_PMD_CRYPTO_SCHEDULER': 'n',
                           'CONFIG_RTE_LIBRTE_REORDER': 'n',
                           'CONFIG_RTE_LIBRTE_PORT': 'n',
                           'CONFIG_RTE_LIBRTE_TABLE': 'n',
                           'CONFIG_RTE_LIBRTE_PIPELINE': 'n',
                           })
    lines += 'CONFIG_RTE_MACHINE={}'.format(dpdk_machine)
    open(dotconfig, 'w', encoding='UTF-8').writelines(lines)
    args.dpdk_target = os.getcwd() + '/build/dpdk'

if args.dpdk_target:
    args.user_cflags = (args.user_cflags +
        ' -DSEASTAR_HAVE_DPDK -I' + args.dpdk_target + '/include ' +
        dpdk_cflags(args.dpdk_target) +
        ' -Wno-error=literal-suffix -Wno-literal-suffix -Wno-invalid-offsetof')
    libs += (' -L' + args.dpdk_target + '/lib ')
    if args.with_osv:
        libs += '-lintel_dpdk -lrt -lm -ldl'
    else:
        libs += '-Wl,--whole-archive -lrte_pmd_vmxnet3_uio -lrte_pmd_i40e -lrte_pmd_ixgbe -lrte_pmd_e1000 -lrte_pmd_ring -lrte_pmd_bnxt -lrte_pmd_cxgbe -lrte_pmd_ena -lrte_pmd_enic -lrte_pmd_fm10k -lrte_pmd_nfp -lrte_pmd_qede -lrte_pmd_sfc_efx -lrte_hash -lrte_kvargs -lrte_mbuf -lrte_ethdev -lrte_eal -lrte_mempool -lrte_mempool_ring -lrte_ring -lrte_cmdline -lrte_cfgfile -Wl,--no-whole-archive -lrt -lm -ldl'

args.user_cflags += ' -I{srcdir}/fmt'.format(**globals())

if not args.staticboost:
    args.user_cflags += ' -DBOOST_TEST_DYN_LINK'

warnings = [w
            for w in warnings
            if warning_supported(warning = w, compiler = args.cxx, flags=args.user_cflags.split())]

warnings = ' '.join(warnings)

dbgflag = debug_flag(args.cxx, flags=args.user_cflags.split()) if args.debuginfo else ''
tests_link_rule = 'link' if args.tests_debuginfo else 'link_stripped'

sanitize_flags = sanitize_vptr_flag(args.cxx, flags=args.user_cflags.split())

visibility_flags = adjust_visibility_flags(args.cxx, flags=args.user_cflags.split())

if not try_compile(args.cxx, source='#include <gnutls/gnutls.h>', flags=args.user_cflags.split()):
    print('Seastar requires gnutls.  Install gnutls-devel/libgnutls-dev')
    sys.exit(1)

if not try_compile(args.cxx, source='#include <gnutls/gnutls.h>\nint x = GNUTLS_NONBLOCK;', flags=args.user_cflags.split()):
    print('Seastar requires gnutls >= 2.8.  Install libgnutls28-dev or later.')
    sys.exit(1)

if not try_compile(args.cxx, source='#include <experimental/string_view>', flags=['-std=gnu++1y'] + args.user_cflags.split()):
    print('Seastar requires g++ >= 4.9.  Install g++-4.9 or later (use --compiler option).')
    sys.exit(1)

if not try_compile(args.cxx, '''#include <boost/version.hpp>\n\
        #if BOOST_VERSION < 105800\n\
        #error "Invalid boost version"\n\
        #endif''', flags=args.user_cflags.split()):
    print("Seastar requires boost >= 1.58")
    sys.exit(1)


modes['debug']['sanitize'] += ' ' + sanitize_flags
modes['release']['opt'] += ' ' + args.user_optflags

def have_hwloc():
    return try_compile(compiler = args.cxx, source = '#include <hwloc.h>\n#include <numa.h>', flags=args.user_cflags.split())

if apply_tristate(args.hwloc, test = have_hwloc,
                  note = 'Note: hwloc-devel/numactl-devel not installed.  No NUMA support.',
                  missing = 'Error: required packages hwloc-devel/numactl-devel not installed.'):
    libs += ' ' + hwloc_libs
    defines.append('SEASTAR_HAVE_HWLOC')
    defines.append('SEASTAR_HAVE_NUMA')

if detect_membarrier(compiler=args.cxx, flags=args.user_cflags.split()):
    defines.append('SEASTAR_HAS_MEMBARRIER')

if try_compile(args.cxx, source = textwrap.dedent('''\
        #include <lz4.h>

        void m() {
            LZ4_compress_default(static_cast<const char*>(0), static_cast<char*>(0), 0, 0);
        }
        '''), flags=args.user_cflags.split()):
    defines.append("SEASTAR_HAVE_LZ4_COMPRESS_DEFAULT")

if try_compile_and_link(args.cxx, flags=['-fsanitize=address'] + args.user_cflags.split(), source = textwrap.dedent('''\
        #include <cstddef>

        extern "C" {
        void __sanitizer_start_switch_fiber(void**, const void*, size_t);
        void __sanitizer_finish_switch_fiber(void*, const void**, size_t*);
        }

        int main() {
            __sanitizer_start_switch_fiber(nullptr, nullptr, 0);
            __sanitizer_finish_switch_fiber(nullptr, nullptr, nullptr);
        }
        ''')):
    defines.append("SEASTAR_HAVE_ASAN_FIBER_SUPPORT")

if args.so:
    args.pie = '-shared'
    args.fpie = '-fpic'
elif args.pie:
    args.pie = '-pie'
    args.fpie = '-fpie'
else:
    args.pie = ''
    args.fpie = ''

defines = ' '.join(['-D' + d for d in defines])

globals().update(vars(args))

total_memory = os.sysconf('SC_PAGE_SIZE') * os.sysconf('SC_PHYS_PAGES')
link_pool_depth = max(int(total_memory / 7e9), 1)

build_modes = modes if args.mode == 'all' else [args.mode]
build_artifacts = all_artifacts if not args.artifacts else args.artifacts
protoc = args.protoc
dpdk_sources = []
if args.dpdk:
    for root, dirs, files in os.walk('dpdk'):
        dpdk_sources += [os.path.join(root, file)
                         for file in files
                         if file.endswith('.h') or file.endswith('.c')]
dpdk_sources = ' '.join(dpdk_sources)

# both source and builddir location
cares_dir = 'c-ares'
cares_lib = 'cares-seastar'
cares_src_lib = cares_dir + '/lib/libcares.a'

if not os.path.exists(cares_dir) or not os.listdir(cares_dir):
    raise Exception(cares_dir + ' is empty. Run "git submodule update --init".')

cares_sources = []
for root, dirs, files in os.walk('c-ares'):
    cares_sources += [os.path.join(root, file)
                      for file in files
                      if file.endswith('.h') or file.endswith('.c')]
cares_sources = ' '.join(cares_sources)
libs += ' -l' + cares_lib

# "libs" contains mostly pre-existing libraries, but if we want to add to
# it a library which we built here, we need to ensure that this library
# gets built before actually using "libs". So let's make a list "built_libs"
# of libraries which are targets built here. These libraries are all relative
# to the current mode's build directory.
built_libs = []
built_libs += ['lib' + cares_lib + '.a']
built_libs += ['fmt/fmt/libfmt.a']

for mode in build_modes:
    configure_fmt(mode, cxx=args.cxx, cc=args.cc)

libs += ' -lfmt'

fmt_deps = []
for dirpath, dirnames, filenames in os.walk('fmt'):
    fmt_deps += [os.path.join(dirpath, filename) for filename in filenames]
fmt_deps = ' '.join(fmt_deps)

outdir = 'build'
buildfile = 'build.ninja'
os.makedirs(outdir, exist_ok = True)
do_sanitize = True
if args.static:
    do_sanitize = False
with open(buildfile, 'w') as f:
    dpdk_deps = ''
    if args.dpdk:
        # fake dependencies on dpdk, so that it is built before anything else
        dpdk_deps = ' {dpdk_target}/include/rte_eal.h {dpdk_target}/lib/librte_eal.a'.format(dpdk_target=args.dpdk_target)
    f.write(textwrap.dedent('''\
        configure_args = {configure_args}
        builddir = {outdir}
        full_builddir = {srcdir}/$builddir
        cxx = {cxx}
        # we disable _FORTIFY_SOURCE because it generates false positives with longjmp() (core/thread.cc)
        cxxflags = -std={cpp_dialect} {dbgflag} {fpie} -Wall -Werror -Wno-error=deprecated-declarations -fvisibility=hidden {visibility_flags} -pthread -I{srcdir} -U_FORTIFY_SOURCE {user_cflags} {warnings} {defines}
        ldflags = {dbgflag} -Wl,--no-as-needed {static} {pie} -fvisibility=hidden {visibility_flags} -pthread {user_ldflags}
        libs = {libs}
        pool link_pool
            depth = {link_pool_depth}
        rule ragel
            # sed away a bug in ragel 7 that emits some extraneous _nfa* variables
            # (the $$ is collapsed to a single one by ninja)
            command = ragel -G2 -o $out $in && sed -i -e '1h;2,$$H;$$!d;g' -re 's/static const char _nfa[^;]*;//g' $out
            description = RAGEL $out
        rule gen
            command = /bin/echo -e $text > $out
            description = GEN $out
        rule swagger
            command = json/json2code.py -f $in -o $out
            description = SWAGGER $out
        rule protobuf
            command = {protoc} --cpp_out=$outdir $in
            description = PROTOC $out
        rule copy_file
            command = cp $in $out
        rule ninja
            command = {ninja_exe} -C $subdir
        ''').format(**globals()))
    if args.dpdk:
        f.write(textwrap.dedent('''\
            rule dpdkmake
                command = make -C build/dpdk CC={args.cc}
            build {dpdk_deps} : dpdkmake {dpdk_sources}
            ''').format(**globals()))
    for mode in build_modes:
        objdeps = {}
        modeval = modes[mode]
        if modeval['sanitize'] and not do_sanitize:
            print('Note: --static disables debug mode sanitizers')
            modeval['sanitize'] = ''
            modeval['sanitize_libs'] = ''
        elif modeval['sanitize']:
            modeval['sanitize'] += ' -DSEASTAR_ASAN_ENABLED'
        f.write(textwrap.dedent('''\
            cxxflags_{mode} = {sanitize} {opt} -I$full_builddir/{mode}/gen -I$full_builddir/{mode}/c-ares
            libs_{mode} = {sanitize_libs} {libs}
            rule cxx.{mode}
              command = $cxx -MD -MT $out -MF $out.d $cxxflags_{mode} $cxxflags -c -o $out $in
              description = CXX $out
              depfile = $out.d
            rule link.{mode}
              command = $cxx  $cxxflags_{mode} -L$builddir/{mode} -L$builddir/{mode}/fmt/fmt $ldflags -o $out $in $libs $libs_{mode} $extralibs
              description = LINK $out
              pool = link_pool
            rule link_stripped.{mode}
              command = $cxx  $cxxflags_{mode} -s -L$builddir/{mode} -L$builddir/{mode}/fmt/fmt $ldflags -o $out $in $libs $libs_{mode} $extralibs
              description = LINK (stripped) $out
              pool = link_pool
            rule ar.{mode}
              command = rm -f $out; ar cr $out $in; ranlib $out
              description = AR $out
            ''').format(mode = mode, **modeval))
        f.write('build {mode}: phony $builddir/{mode}/lib{cares_lib}.a {artifacts}\n'.format(mode = mode, cares_lib=cares_lib,
            artifacts = str.join(' ', ('$builddir/' + mode + '/' + x for x in build_artifacts))))
        f.write(textwrap.dedent('''\
              rule caresmake_{mode}
                command = make -C build/{mode}/{cares_dir} CC={args.cc}
              rule carescmake_{mode}
                command = mkdir -p $builddir/{mode}/{cares_dir} && cd $builddir/{mode}/{cares_dir} && CC={args.cc} cmake {cares_opts} {srcdir}/$in
              build $builddir/{mode}/{cares_dir}/Makefile : carescmake_{mode} {cares_dir}
              build $builddir/{mode}/{cares_dir}/ares_build.h : phony $builddir/{mode}/{cares_dir}/Makefile
              build $builddir/{mode}/{cares_src_lib} : caresmake_{mode} $builddir/{mode}/{cares_dir}/Makefile | {cares_sources}
              build $builddir/{mode}/lib{cares_lib}.a : copy_file $builddir/{mode}/{cares_src_lib}
            ''').format(cares_opts=(modeval['cares_opts']), **globals()))
        objdeps['$builddir/' + mode + '/net/dns.o'] = ' $builddir/' + mode + '/' + cares_dir + '/ares_build.h'
        compiles = {}
        ragels = {}
        swaggers = {}
        protobufs = {}
        for binary in build_artifacts:
            srcs = deps[binary]
            objs = ['$builddir/' + mode + '/' + src.replace('.cc', '.o')
                    for src in srcs
                    if src.endswith('.cc')]
            objs += ['$builddir/' + mode + '/gen/' + src.replace('.proto', '.pb.o')
                    for src in srcs
                    if src.endswith('.proto')]
            if binary.endswith('.pc'):
                vars = modeval.copy()
                vars.update(globals())
                pc = textwrap.dedent('''\
                        Name: Seastar
                        URL: http://seastar-project.org/
                        Description: Advanced C++ framework for high-performance server applications on modern hardware.
                        Version: 1.0
                        Libs: -L$full_builddir/{mode} -L$full_builddir/{mode}/fmt/fmt -Wl,--whole-archive,-lseastar,--no-whole-archive -lfmt $cxxflags $cxflags_{mode} -Wl,--no-as-needed {static} {pie} {user_ldflags} {sanitize_libs} {libs}
                        Cflags: $cxxflags $cxxflags_{mode}
                        ''').format(**vars)
                f.write('build $builddir/{}/{}: gen\n  text = {}\n'.format(mode, binary, repr(pc)))
            elif binary == 'fmt/fmt/libfmt.a':
                f.write('build $builddir/{}/fmt/fmt//libfmt.a: ninja | {}\n'.format(mode, fmt_deps))
                f.write('  subdir=build/{}/fmt\n'.format(mode))
            elif binary.endswith('.a'):
                f.write('build $builddir/{}/{}: ar.{} {}\n'.format(mode, binary, mode, str.join(' ', objs)))
            else:
                libdeps = str.join(' ', ('$builddir/{}/{}'.format(mode, i) for i in built_libs))
                test_extralibs = [maybe_static(args.staticyamlcpp, '-lyaml-cpp')]
                if binary.startswith('tests/'):
                    if binary in boost_tests:
                        test_extralibs += [maybe_static(args.staticboost, '-lboost_unit_test_framework')]
                    # Our code's debugging information is huge, and multiplied
                    # by many tests yields ridiculous amounts of disk space.
                    # So we strip the tests by default; The user can very
                    # quickly re-link the test unstripped by adding a "_g"
                    # to the test name, e.g., "ninja build/release/testname_g"
                    f.write('build $builddir/{}/{}: {}.{} {} | {} {}\n'.format(mode, binary, tests_link_rule, mode, str.join(' ', objs), dpdk_deps, libdeps))
                    f.write('  extralibs = {}\n'.format(' '.join(test_extralibs)))
                    f.write('build $builddir/{}/{}_g: link.{} {} | {} {}\n'.format(mode, binary, mode, str.join(' ', objs), dpdk_deps, libdeps))
                    f.write('  extralibs = {}\n'.format(' '.join(test_extralibs)))
                else:
                    f.write('build $builddir/{}/{}: link.{} {} | {} {} $builddir/{}/lib{}.a $builddir/{}/fmt/fmt/libfmt.a\n'.format(mode, binary, mode, str.join(' ', objs), dpdk_deps, libdeps, mode, cares_lib, mode))
                    if binary in extralibs.keys():
                        app_extralibs = extralibs[binary]
                        f.write('  extralibs = {}\n'.format(' '.join(app_extralibs)))

            for src in srcs:
                if src.endswith('.cc'):
                    obj = '$builddir/' + mode + '/' + src.replace('.cc', '.o')
                    compiles[obj] = src
                elif src.endswith('.proto'):
                    hh = '$builddir/' + mode + '/gen/' + src.replace('.proto', '.pb.h')
                    protobufs[hh] = src
                    compiles[hh.replace('.h', '.o')] = hh.replace('.h', '.cc')
                elif src.endswith('.rl'):
                    hh = '$builddir/' + mode + '/gen/' + src.replace('.rl', '.hh')
                    ragels[hh] = src
                elif src.endswith('.json'):
                    hh = '$builddir/' + mode + '/gen/' + src + '.hh'
                    swaggers[hh] = src
                else:
                    raise Exception('No rule for ' + src)
        for obj in compiles:
            src = compiles[obj]
            gen_headers = list(ragels.keys()) + list(swaggers.keys()) + list(protobufs.keys())
            f.write('build {}: cxx.{} {} || {} \n'.format(obj, mode, src, ' '.join(gen_headers) + dpdk_deps + objdeps.get(obj, '')))
        for hh in ragels:
            src = ragels[hh]
            f.write('build {}: ragel {}\n'.format(hh, src))
        for hh in swaggers:
            src = swaggers[hh]
            f.write('build {}: swagger {} | json/json2code.py\n'.format(hh,src))
        for pb in protobufs:
            src = protobufs[pb]
            c_pb = pb.replace('.h','.cc')
            outd = os.path.dirname(os.path.dirname(pb))
            f.write('build {} {}: protobuf {}\n  outdir = {}\n'.format(c_pb, pb, src, outd))

    f.write(textwrap.dedent('''\
        rule configure
          command = python3 configure.py $configure_args
          generator = 1
        build build.ninja: configure | configure.py
        rule cscope
            command = find -name '*.[chS]' -o -name "*.cc" -o -name "*.hh" | cscope -bq -i-
            description = CSCOPE
        build cscope: cscope
        rule md2html
            command = doc/md2html "$in" "$out"
            description = PANDOC $out
        rule md2pdf
            command = doc/md2pdf "$in" "$out"
            description = PANDOC $out
        rule htmlsplit
            command = cd doc; ./htmlsplit.py
            description = HTMLSPLIT $out
        build doc/tutorial.html: md2html doc/tutorial.md
        build doc/tutorial.pdf: md2pdf doc/tutorial.md
        build doc/split: htmlsplit doc/tutorial.html
        default {modes_list}
        ''').format(modes_list = ' '.join(build_modes), **globals()))
