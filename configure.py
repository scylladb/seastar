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

configure_args = str.join(' ', [shlex.quote(x) for x in sys.argv[1:]])

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
    with tempfile.NamedTemporaryFile() as sfile:
        sfile.file.write(bytes(source, 'utf-8'))
        sfile.file.flush()
        return subprocess.call([compiler, '-x', 'c++', '-o', '/dev/null', '-c', sfile.name] + flags,
                               stdout = subprocess.DEVNULL,
                               stderr = subprocess.DEVNULL) == 0

def try_compile_and_run(compiler, flags, source, env = {}):
    mktemp = tempfile.NamedTemporaryFile
    with mktemp() as sfile, mktemp(mode='rb') as xfile:
        sfile.file.write(bytes(source, 'utf-8'))
        sfile.file.flush()
        xfile.file.close()
        if subprocess.call([compiler, '-x', 'c++', '-o', xfile.name, sfile.name] + flags,
                            stdout = subprocess.DEVNULL, stderr = subprocess.DEVNULL) != 0:
            # The compiler may delete the target on failure, and lead to
            # NamedTemporaryFile's destructor throwing an exception.
            open(xfile.name, 'a').close()
            return False
        e = os.environ.copy()
        e.update(env)
        env = e
        return subprocess.call([xfile.name], stdout = subprocess.DEVNULL, stderr = subprocess.DEVNULL, env=env) == 0

def warning_supported(warning, compiler):
    # gcc ignores -Wno-x even if it is not supported
    adjusted = re.sub('^-Wno-', '-W', warning)
    return try_compile(flags = [adjusted, '-Werror'], compiler = compiler)

def debug_flag(compiler):
    src_with_auto = textwrap.dedent('''\
        template <typename T>
        struct x { auto f() {} };

        x<int> a;
        ''')
    if try_compile(source = src_with_auto, flags = ['-g', '-std=gnu++1y'], compiler = compiler):
        return '-g'
    else:
        print('Note: debug information disabled; upgrade your compiler')
        return ''

def sanitize_vptr_flag(compiler):
    # https://gcc.gnu.org/bugzilla/show_bug.cgi?id=67258
    if (not try_compile(compiler, flags=['-fsanitize=vptr'])
        or (try_compile_and_run(compiler, flags=['-fsanitize=undefined', '-fno-sanitize-recover'],
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

modes = {
    'debug': {
        'sanitize': '-fsanitize=address -fsanitize=leak -fsanitize=undefined',
        'sanitize_libs': '-lasan -lubsan',
        'opt': '-O0 -DDEBUG -DDEBUG_SHARED_PTR -DDEFAULT_ALLOCATOR -DSEASTAR_THREAD_STACK_GUARDS',
        'libs': '',
    },
    'release': {
        'sanitize': '',
        'sanitize_libs': '',
        'opt': '-O2',
        'libs': '',
    },
}

tests = [
    'tests/fileiotest',
    'tests/directory_test',
    'tests/linecount',
    'tests/echotest',
    'tests/l3_test',
    'tests/ip_test',
    'tests/timertest',
    'tests/tcp_test',
    'tests/futures_test',
    'tests/alloc_test',
    'tests/foreign_ptr_test',
    'tests/smp_test',
    'tests/thread_test',
    'tests/thread_context_switch',
    'tests/udp_server',
    'tests/udp_client',
    'tests/blkdiscard_test',
    'tests/sstring_test',
    'tests/unwind_test',
    'tests/defer_test',
    'tests/httpd',
    'tests/memcached/test_ascii_parser',
    'tests/tcp_sctp_server',
    'tests/tcp_sctp_client',
    'tests/allocator_test',
    'tests/output_stream_test',
    'tests/udp_zero_copy',
    'tests/shared_ptr_test',
    'tests/weak_ptr_test',
    'tests/slab_test',
    'tests/fstream_test',
    'tests/distributed_test',
    'tests/rpc',
    'tests/semaphore_test',
    'tests/packet_test',
    'tests/tls_test',
    'tests/fair_queue_test',
    'tests/rpc_test',
    'tests/connect_test',
    'tests/chunked_fifo_test',
    'tests/scollectd_test',
    'tests/perf/perf_fstream',
    'tests/json_formatter_test',
    ]

apps = [
    'apps/httpd/httpd',
    'apps/seawreck/seawreck',
    'apps/fair_queue_tester/fair_queue_tester',
    'apps/memcached/memcached',
    'apps/iotune/iotune',
    ]

all_artifacts = apps + tests + ['libseastar.a', 'seastar.pc']

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
arg_parser.add_argument('--compiler', action = 'store', dest = 'cxx', default = 'g++',
                        help = 'C++ compiler path')
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
add_tristate(arg_parser, name = 'hwloc', dest = 'hwloc', help = 'hwloc support')
add_tristate(arg_parser, name = 'xen', dest = 'xen', help = 'Xen support')
args = arg_parser.parse_args()

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
    ]

core = [
    'core/reactor.cc',
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
    'util/conversions.cc',
    'util/log.cc',
    'net/packet.cc',
    'net/posix-stack.cc',
    'net/net.cc',
    'net/stack.cc',
    'rpc/rpc.cc',
    'rpc/lz4_compressor.cc',
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

defines = ['FMT_HEADER_ONLY']
# Include -lgcc_s before -lunwind to work around for https://savannah.nongnu.org/bugs/?48486. See https://github.com/scylladb/scylla/issues/1725.
libs = ' '.join(['-laio',
                 maybe_static(args.staticboost,
                              '-lboost_program_options -lboost_system -lboost_filesystem'),
                 '-lstdc++ -lm',
                 maybe_static(args.staticboost, '-lboost_thread'),
                 '-lcryptopp -lrt -lgnutls -lgnutlsxx -llz4 -lprotobuf -ldl -lgcc_s -lunwind',
                 ])

boost_unit_test_lib = maybe_static(args.staticboost, '-lboost_unit_test_framework')


hwloc_libs = '-lhwloc -lnuma -lpciaccess -lxml2 -lz'
xen_used = False
def have_xen():
    source  = '#include <stdint.h>\n'
    source += '#include <xen/xen.h>\n'
    source += '#include <xen/sys/evtchn.h>\n'
    source += '#include <xen/sys/gntdev.h>\n'
    source += '#include <xen/sys/gntalloc.h>\n'

    return try_compile(compiler = args.cxx, source = source)

if apply_tristate(args.xen, test = have_xen,
                  note = 'Note: xen-devel not installed.  No Xen support.',
                  missing = 'Error: required package xen-devel not installed.'):
    libs += ' -lxenstore'
    defines.append("HAVE_XEN")
    libnet += [ 'net/xenfront.cc' ]
    core += [
                'core/xen/xenstore.cc',
                'core/xen/gntalloc.cc',
                'core/xen/evtchn.cc',
            ]
    xen_used=True

if xen_used and args.dpdk_target:
    print("Error: only xen or dpdk can be used, not both.")
    sys.exit(1)

if args.staticcxx:
    libs = libs.replace('-lstdc++', '')
    libs += ' -static-libgcc -static-libstdc++'

if args.staticcxx or args.static:
    defines.append("NO_EXCEPTION_INTERCEPT");

memcache_base = [
    'apps/memcached/ascii.rl'
] + libnet + core

deps = {
    'libseastar.a' : core + libnet + http + protobuf + prometheus,
    'seastar.pc': [],
    'apps/httpd/httpd': ['apps/httpd/demo.json', 'apps/httpd/main.cc'] + http + libnet + core,
    'apps/memcached/memcached': ['apps/memcached/memcache.cc'] + memcache_base,
    'tests/memcached/test_ascii_parser': ['tests/memcached/test_ascii_parser.cc'] + memcache_base,
    'tests/fileiotest': ['tests/fileiotest.cc'] + core,
    'tests/directory_test': ['tests/directory_test.cc'] + core,
    'tests/linecount': ['tests/linecount.cc'] + core,
    'tests/echotest': ['tests/echotest.cc'] + core + libnet,
    'tests/l3_test': ['tests/l3_test.cc'] + core + libnet,
    'tests/ip_test': ['tests/ip_test.cc'] + core + libnet,
    'tests/tcp_test': ['tests/tcp_test.cc'] + core + libnet,
    'tests/timertest': ['tests/timertest.cc'] + core,
    'tests/futures_test': ['tests/futures_test.cc'] + core,
    'tests/alloc_test': ['tests/alloc_test.cc'] + core,
    'tests/foreign_ptr_test': ['tests/foreign_ptr_test.cc'] + core,
    'tests/semaphore_test': ['tests/semaphore_test.cc'] + core,
    'tests/smp_test': ['tests/smp_test.cc'] + core,
    'tests/thread_test': ['tests/thread_test.cc'] + core,
    'tests/thread_context_switch': ['tests/thread_context_switch.cc'] + core,
    'tests/udp_server': ['tests/udp_server.cc'] + core + libnet,
    'tests/udp_client': ['tests/udp_client.cc'] + core + libnet,
    'tests/tcp_sctp_server': ['tests/tcp_sctp_server.cc'] + core + libnet,
    'tests/tcp_sctp_client': ['tests/tcp_sctp_client.cc'] + core + libnet,
    'tests/tls_test': ['tests/tls_test.cc'] + core + libnet,
    'tests/fair_queue_test': ['tests/fair_queue_test.cc'] + core,
    'apps/seawreck/seawreck': ['apps/seawreck/seawreck.cc', 'http/http_response_parser.rl'] + core + libnet,
    'apps/fair_queue_tester/fair_queue_tester': ['apps/fair_queue_tester/fair_queue_tester.cc'] + core,
    'apps/iotune/iotune': ['apps/iotune/iotune.cc', 'apps/iotune/fsqual.cc'] + ['core/resource.cc'],
    'tests/blkdiscard_test': ['tests/blkdiscard_test.cc'] + core,
    'tests/sstring_test': ['tests/sstring_test.cc'] + core,
    'tests/unwind_test': ['tests/unwind_test.cc'] + core,
    'tests/defer_test': ['tests/defer_test.cc'] + core,
    'tests/httpd': ['tests/httpd.cc'] + http + core,
    'tests/allocator_test': ['tests/allocator_test.cc'] + core,
    'tests/output_stream_test': ['tests/output_stream_test.cc'] + core + libnet,
    'tests/udp_zero_copy': ['tests/udp_zero_copy.cc'] + core + libnet,
    'tests/shared_ptr_test': ['tests/shared_ptr_test.cc'] + core,
    'tests/weak_ptr_test': ['tests/weak_ptr_test.cc'] + core,
    'tests/slab_test': ['tests/slab_test.cc'] + core,
    'tests/fstream_test': ['tests/fstream_test.cc'] + core,
    'tests/distributed_test': ['tests/distributed_test.cc'] + core,
    'tests/rpc': ['tests/rpc.cc'] + core + libnet,
    'tests/rpc_test': ['tests/rpc_test.cc'] + core + libnet,
    'tests/packet_test': ['tests/packet_test.cc'] + core + libnet,
    'tests/connect_test': ['tests/connect_test.cc'] + core + libnet,
    'tests/chunked_fifo_test': ['tests/chunked_fifo_test.cc'] + core,
    'tests/scollectd_test': ['tests/scollectd_test.cc'] + core,
    'tests/perf/perf_fstream': ['tests/perf/perf_fstream.cc'] + core,
    'tests/json_formatter_test': ['tests/json_formatter_test.cc'] + core + http,
}

boost_tests = [
    'tests/memcached/test_ascii_parser',
    'tests/fileiotest',
    'tests/futures_test',
    'tests/alloc_test',
    'tests/foreign_ptr_test',
    'tests/semaphore_test',
    'tests/thread_test',
    'tests/tls_test',
    'tests/fair_queue_test',
    'tests/httpd',
    'tests/output_stream_test',
    'tests/fstream_test',
    'tests/rpc_test',
    'tests/connect_test',
    'tests/scollectd_test',
    'tests/json_formatter_test',
    ]

for bt in boost_tests:
    deps[bt] += boost_test_lib

warnings = [
    '-Wno-mismatched-tags',                 # clang-only
    '-Wno-pessimizing-move',                # clang-only: moving a temporary object prevents copy elision
    '-Wno-redundant-move',                  # clang-only: redundant move in return statement
    '-Wno-inconsistent-missing-override',   # clang-only: 'x' overrides a member function but is not marked 'override'
    '-Wno-unused-private-field',            # clang-only: private field 'x' is not used
    '-Wno-unknown-attributes',              # clang-only: unknown attribute 'x' ignored (x in this case is gnu::externally_visible)
    '-Wno-unneeded-internal-declaration',   # clang-only: 'x' function 'x' declared in header file shouldb e declared 'x'
    '-Wno-undefined-inline',                # clang-only: inline function 'x' is not defined
    '-Wno-overloaded-virtual',              # clang-only: 'x' hides overloaded virtual functions
    ]

# The "--with-osv=<path>" parameter is a shortcut for a bunch of other
# settings:
if args.with_osv:
    args.so = True
    args.hwloc = False
    args.user_cflags = (args.user_cflags +
        ' -DDEFAULT_ALLOCATOR -fvisibility=default -DHAVE_OSV -I' +
        args.with_osv + ' -I' + args.with_osv + '/include -I' +
        args.with_osv + '/arch/x64')

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
        ' -DHAVE_DPDK -I' + args.dpdk_target + '/include ' +
        dpdk_cflags(args.dpdk_target) +
        ' -Wno-error=literal-suffix -Wno-literal-suffix -Wno-invalid-offsetof')
    libs += (' -L' + args.dpdk_target + '/lib ')
    if args.with_osv:
        libs += '-lintel_dpdk -lrt -lm -ldl'
    else:
        libs += '-Wl,--whole-archive -lrte_pmd_vmxnet3_uio -lrte_pmd_i40e -lrte_pmd_ixgbe -lrte_pmd_e1000 -lrte_pmd_ring -Wl,--no-whole-archive -lrte_hash -lrte_kvargs -lrte_mbuf -lethdev -lrte_eal -lrte_malloc -lrte_mempool -lrte_ring -lrte_cmdline -lrte_cfgfile -lrt -lm -ldl'

args.user_cflags += ' -Ifmt'

if not args.staticboost:
    args.user_cflags += ' -DBOOST_TEST_DYN_LINK'

warnings = [w
            for w in warnings
            if warning_supported(warning = w, compiler = args.cxx)]

warnings = ' '.join(warnings)

dbgflag = debug_flag(args.cxx) if args.debuginfo else ''
tests_link_rule = 'link' if args.tests_debuginfo else 'link_stripped'

sanitize_flags = sanitize_vptr_flag(args.cxx)

if not try_compile(args.cxx, '#include <gnutls/gnutls.h>'):
    print('Seastar requires gnutls.  Install gnutls-devel/libgnutls-dev')
    sys.exit(1)

if not try_compile(args.cxx, '#include <gnutls/gnutls.h>\nint x = GNUTLS_NONBLOCK;'):
    print('Seastar requires gnutls >= 2.8.  Install libgnutls28-dev or later.')
    sys.exit(1)

if not try_compile(args.cxx, '#include <experimental/string_view>', ['-std=gnu++1y']):
    print('Seastar requires g++ >= 4.9.  Install g++-4.9 or later (use --compiler option).')
    sys.exit(1)

modes['debug']['sanitize'] += ' ' + sanitize_flags

def have_hwloc():
    return try_compile(compiler = args.cxx, source = '#include <hwloc.h>\n#include <numa.h>')

if apply_tristate(args.hwloc, test = have_hwloc,
                  note = 'Note: hwloc-devel/numactl-devel not installed.  No NUMA support.',
                  missing = 'Error: required packages hwloc-devel/numactl-devel not installed.'):
    libs += ' ' + hwloc_libs
    defines.append('HAVE_HWLOC')
    defines.append('HAVE_NUMA')

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

dpdk_sources = []
if args.dpdk:
    for root, dirs, files in os.walk('dpdk'):
        dpdk_sources += [os.path.join(root, file)
                         for file in files
                         if file.endswith('.h') or file.endswith('.c')]
dpdk_sources = ' '.join(dpdk_sources)

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
        cxx = {cxx}
        # we disable _FORTIFY_SOURCE because it generates false positives with longjmp() (core/thread.cc)
        cxxflags = -std=gnu++1y {dbgflag} {fpie} -Wall -Werror -fvisibility=hidden -pthread -I. -U_FORTIFY_SOURCE {user_cflags} {warnings} {defines}
        ldflags = {dbgflag} -Wl,--no-as-needed {static} {pie} -fvisibility=hidden -pthread {user_ldflags}
        libs = {libs}
        pool link_pool
            depth = {link_pool_depth}
        rule ragel
            command = ragel -G2 -o $out $in
            description = RAGEL $out
        rule gen
            command = /bin/echo -e $text > $out
            description = GEN $out
        rule swagger
            command = json/json2code.py -f $in -o $out
            description = SWAGGER $out
        rule protobuf
            command = protoc --cpp_out=$outdir $in
            description = PROTOC $out
        ''').format(**globals()))
    if args.dpdk:
        f.write(textwrap.dedent('''\
            rule dpdkmake
                command = make -C build/dpdk
            build {dpdk_deps} : dpdkmake {dpdk_sources}
            ''').format(**globals()))
    for mode in build_modes:
        modeval = modes[mode]
        if modeval['sanitize'] and not do_sanitize:
            print('Note: --static disables debug mode sanitizers')
            modeval['sanitize'] = ''
            modeval['sanitize_libs'] = ''
        elif modeval['sanitize']:
            modeval['sanitize'] += ' -DASAN_ENABLED'
        f.write(textwrap.dedent('''\
            cxxflags_{mode} = {sanitize} {opt} -I $builddir/{mode}/gen
            libs_{mode} = {sanitize_libs} {libs}
            rule cxx.{mode}
              command = $cxx -MMD -MT $out -MF $out.d $cxxflags_{mode} $cxxflags -c -o $out $in
              description = CXX $out
              depfile = $out.d
            rule link.{mode}
              command = $cxx  $cxxflags_{mode} $ldflags -o $out $in $libs $libs_{mode} $extralibs
              description = LINK $out
              pool = link_pool
            rule link_stripped.{mode}
              command = $cxx  $cxxflags_{mode} -s $ldflags -o $out $in $libs $libs_{mode} $extralibs
              description = LINK (stripped) $out
              pool = link_pool
            rule ar.{mode}
              command = rm -f $out; ar cr $out $in; ranlib $out
              description = AR $out
            ''').format(mode = mode, **modeval))
        f.write('build {mode}: phony {artifacts}\n'.format(mode = mode,
            artifacts = str.join(' ', ('$builddir/' + mode + '/' + x for x in build_artifacts))))
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
                        Libs: -L{srcdir}/{builddir} -Wl,--whole-archive,-lseastar,--no-whole-archive {dbgflag} -Wl,--no-as-needed {static} {pie} -fvisibility=hidden -pthread {user_ldflags} {sanitize_libs} {libs}
                        Cflags: -std=gnu++1y {dbgflag} {fpie} -Wall -Werror -fvisibility=hidden -pthread -I{srcdir} -I{srcdir}/fmt -I{srcdir}/{builddir}/gen {user_cflags} {warnings} {defines} {sanitize} {opt}
                        ''').format(builddir = 'build/' + mode, srcdir = os.getcwd(), **vars)
                f.write('build $builddir/{}/{}: gen\n  text = {}\n'.format(mode, binary, repr(pc)))
            elif binary.endswith('.a'):
                f.write('build $builddir/{}/{}: ar.{} {}\n'.format(mode, binary, mode, str.join(' ', objs)))
            else:
                extralibs = []
                if binary.startswith('tests/'):
                    if binary in boost_tests:
                        extralibs += [maybe_static(args.staticboost, '-lboost_unit_test_framework')]
                    # Our code's debugging information is huge, and multiplied
                    # by many tests yields ridiculous amounts of disk space.
                    # So we strip the tests by default; The user can very
                    # quickly re-link the test unstripped by adding a "_g"
                    # to the test name, e.g., "ninja build/release/testname_g"
                    f.write('build $builddir/{}/{}: {}.{} {} | {}\n'.format(mode, binary, tests_link_rule, mode, str.join(' ', objs), dpdk_deps))
                    f.write('  extralibs = {}\n'.format(' '.join(extralibs)))
                    f.write('build $builddir/{}/{}_g: link.{} {} | {}\n'.format(mode, binary, mode, str.join(' ', objs), dpdk_deps))
                    f.write('  extralibs = {}\n'.format(' '.join(extralibs)))
                else:
                    f.write('build $builddir/{}/{}: link.{} {} | {}\n'.format(mode, binary, mode, str.join(' ', objs), dpdk_deps))

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
            f.write('build {}: cxx.{} {} || {} \n'.format(obj, mode, src, ' '.join(gen_headers) + dpdk_deps))
        for hh in ragels:
            src = ragels[hh]
            f.write('build {}: ragel {}\n'.format(hh, src))
        for hh in swaggers:
            src = swaggers[hh]
            f.write('build {}: swagger {}\n'.format(hh,src))
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
            command = pandoc --self-contained --toc -c doc/template.css -V documentclass=report --chapters --number-sections -f markdown_github+pandoc_title_block --highlight-style tango $in -o $out
            description = PANDOC $out
        rule md2pdf
            command = pandoc -f markdown_github+pandoc_title_block --highlight-style tango --template=doc/template.tex $in -o $out
            description = PANDOC $out
        build doc/tutorial.html: md2html doc/tutorial.md
        build doc/tutorial.pdf: md2pdf doc/tutorial.md
        default {modes_list}
        ''').format(modes_list = ' '.join(build_modes), **globals()))
