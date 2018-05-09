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
import os
import sys
import argparse
import subprocess
import signal
import re
import seastar_cmake

boost_tests = [
    'alloc_test',
    'futures_test',
    'thread_test',
    'memcached/memcached_ascii_parser_test',
    'sstring_test',
    'unwind_test',
    'defer_test',
    'output_stream_test',
    'httpd_test',
    'fstream_test',
    'foreign_ptr_test',
    'semaphore_test',
    'expiring_fifo_test',
    'shared_ptr_test',
    'weak_ptr_test',
    'file_io_test',
    'packet_test',
    'tls_test',
    'rpc_test',
    'connect_test',
    'json_formatter_test',
    'execution_stage_test',
    'lowres_clock_test',
    'program_options_test',
    'tuple_utils_test',
    'noncopyable_function_test',
    'abort_source_test',
]

other_tests = [
    'smp_test',
    'timer_test',
    'directory_test',
    'thread_context_switch_test',
    'fair_queue_test',
    'alien_test',
]

last_len = 0

def print_status_short(msg):
    global last_len
    print('\r' + ' '*last_len, end='')
    last_len = len(msg)
    print('\r' + msg, end='')

print_status_verbose = print

class Alarm(Exception):
    pass
def alarm_handler(signum, frame):
    raise Alarm

def make_build_path(mode, *suffixes):
    return os.path.join('build', mode, *suffixes)

if __name__ == "__main__":
    all_modes = ['debug', 'release']

    parser = argparse.ArgumentParser(description="Seastar test runner")
    parser.add_argument('--fast',  action="store_true", help="Run only fast tests")
    parser.add_argument('--name',  action="store", help="Run only test whose name contains given string")
    parser.add_argument('--mode', choices=all_modes, help="Run only tests for given build mode")
    parser.add_argument('--timeout', action="store",default="300",type=int, help="timeout value for test execution")
    parser.add_argument('--jenkins', action="store",help="jenkins output file prefix")
    parser.add_argument('--verbose', '-v', action = 'store_true', default = False,
                        help = 'Verbose reporting')
    parser.add_argument('--cmake', action='store_true', help='Use the CMake test-runner (CTest)')
    args = parser.parse_args()

    # Forwarding to CMake.
    if args.cmake:
        MODES = [args.mode] if args.mode else seastar_cmake.SUPPORTED_MODES

        def run_tests(mode):
            BUILD_PATH = seastar_cmake.BUILD_PATHS[mode]

            # For convenience.
            tr = seastar_cmake.translate_arg

            TRANSLATED_CMAKE_ARGS = [
                tr(args.fast, 'EXECUTE_ONLY_FAST_TESTS'),
                tr(args.jenkins, 'JENKINS', value_when_none=''),
            ]

            # Modify the existing build by pointing to the build directory.
            CMAKE_ARGS = seastar_cmake.CMAKE_BASIC_ARGS + [BUILD_PATH] + TRANSLATED_CMAKE_ARGS
            print(CMAKE_ARGS)
            subprocess.check_call(CMAKE_ARGS, shell=False, cwd=BUILD_PATH)

            TRANSLATED_CTEST_ARGS = [
                '--timeout', args.timeout
            ] + ['--verbose'] if args.verbose else [
            ] + ['-R', args.name] if args.name else [
            ]

            CTEST_ARGS = ['ctest', BUILD_PATH] + TRANSLATED_CTEST_ARGS
            print(CTEST_ARGS)
            subprocess.check_call(CTEST_ARGS, shell=False, cwd=BUILD_PATH)

        for mode in MODES:
            run_tests(mode)

        sys.exit(0)

    print_status = print_status_verbose if args.verbose else print_status_short

    # Run on 2 shard - it should be enough
    cpu_count = 2

    test_to_run = []
    modes_to_run = all_modes if not args.mode else [args.mode]
    for mode in modes_to_run:
        prefix = os.path.join('build', mode, 'tests')
        for test in other_tests:
            test_to_run.append((os.path.join(prefix, test),'other'))
        for test in boost_tests:
            test_to_run.append((os.path.join(prefix, test),'boost'))
        memcached_path = make_build_path(mode, 'apps', 'memcached', 'memcached')
        test_to_run.append(('tests/memcached/test.py --memcached ' + memcached_path + (' --fast' if args.fast else ''),'other'))
        test_to_run.append((os.path.join(prefix, 'distributed_test'),'other'))


        allocator_test_path = os.path.join(prefix, 'allocator_test')
        if args.fast:
            if mode == 'debug':
                test_to_run.append((allocator_test_path + ' --iterations 5','other'))
            else:
                test_to_run.append((allocator_test_path + ' --time 0.1','other'))
        else:
            test_to_run.append((allocator_test_path,'other'))

    if args.name:
        test_to_run = [t for t in test_to_run if args.name in t[0]]


    all_ok = True

    n_total = len(test_to_run)
    env = os.environ
    # disable false positive due to new (with_alignment(...)) ...
    env['ASAN_OPTIONS'] = 'alloc_dealloc_mismatch=0'
    for n, test in enumerate(test_to_run):
        path = test[0]
        prefix = '[%d/%d]' % (n + 1, n_total)
        print_status('%s RUNNING %s' % (prefix, path))
        signal.signal(signal.SIGALRM, alarm_handler)
        if args.jenkins and test[1] == 'boost':
           mode = 'release'
           if test[0].startswith(os.path.join('build','debug')):
              mode = 'debug'
           xmlout = args.jenkins+"."+mode+"."+os.path.basename(test[0])+".boost.xml"
           path = path + " --output_format=XML --log_level=all --report_level=no --log_sink=" + xmlout
           print(path)
        if os.path.isfile('tmp.out'):
           os.remove('tmp.out')
        outf=open('tmp.out','w')

        # Limit shards count
        if test[1] == 'boost':
            path = path + " -- --smp={}".format(cpu_count)
        else:
            if not re.search("tests/memcached/test.py", path):
                if re.search("allocator_test", path) or re.search("fair_queue_test", path):
                    path = path + " -- --smp={}".format(cpu_count)
                else:
                    path = path + " --smp={}".format(cpu_count)

        proc = subprocess.Popen(path.split(' '), stdout=outf, stderr=subprocess.PIPE, env=env,preexec_fn=os.setsid)
        signal.alarm(args.timeout)
        err = None
        out = None
        try:
            out,err = proc.communicate()
            signal.alarm(0)
        except:
            os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
            proc.kill()
            proc.returncode = -1
        finally:
            outf.close();
            if proc.returncode:
                print_status('FAILED: %s\n' % (path))
                if proc.returncode == -1:
                    print_status('TIMED OUT\n')
                else:
                    print_status('  with error code {code}\n'.format(code=proc.returncode))
                print('=== stdout START ===')
                with open('tmp.out') as outf:
                   for line in outf:
                       print(line)
                print('=== stdout END ===')
                if err:
                    print('=== stderr START ===')
                    print(err.decode())
                    print('=== stderr END ===')
                all_ok = False
            else:
                print_status('%s PASSED %s' % (prefix, path))

    if all_ok:
        print('\nOK.')
    else:
        print_status('')
        sys.exit(1)
