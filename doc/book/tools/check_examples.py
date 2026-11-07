#!/usr/bin/python3 -I
# Build and run the book's example programs (examples/*/*.cc) and every
# version of Shoal (shoal/v*/), to check that the code the book shows really
# compiles and works.
#
# Usage: check_examples.py [--seastar CXX[:STD]=PREFIX[:PREFIX...]]...
#                          [--std 23,26] [--out DIR] [NAME...]
#
# Each --seastar names a compiler and the Seastar built with it: the
# installation prefix, followed by any other prefixes its dependencies need
# (for cooked dependencies, the build directory's _cooking/installed). With
# :STD, that Seastar was built with that C++ standard, and the programs are
# built against it only with that standard; without it, with every standard
# in --std. The book promises that its code works with both GCC and Clang, in
# C++23 and C++26, and Seastar's CMake files warn when a program's standard
# isn't Seastar's, so the complete check uses four Seastars:
#
#   check_examples.py \
#     --seastar g++:23=$GCC23/install:$GCC23/build/release/_cooking/installed \
#     --seastar g++:26=$GCC26/install:$GCC26/build/release/_cooking/installed \
#     --seastar clang++:23=$CLANG23/install:$CLANG23/build/release/_cooking/installed \
#     --seastar clang++:26=$CLANG26/install:$CLANG26/build/release/_cooking/installed
#
# Without --seastar, it uses g++ and the Seastar that pkg-config and CMake
# find (PKG_CONFIG_PATH, CMAKE_PREFIX_PATH).
#
# For each compiler, every example and every Shoal version is built with each
# C++ standard: the examples with the compiler and pkg-config's flags, Shoal
# with CMake and find_package(Seastar). Then the programs built with the first
# standard are run: those that end by themselves are run and their output
# checked, and the servers are started, tested with a client, and killed.
# NAMEs limit the check to the examples with these file names (without .cc),
# or to Shoal versions (shoal-v0, ...).

import argparse
import os
import shutil
import re
import signal
import socket
import subprocess
import sys
import time
from pathlib import Path

book = Path(__file__).resolve().parent.parent

def run(binary, *args, timeout=60, stderr=False):
    """Runs a program that ends by itself, and returns its output: stdout, or
    with stderr=True, stderr (where Seastar's logger writes)."""
    p = subprocess.run([binary, *args], capture_output=True, text=True, timeout=timeout)
    if p.returncode != 0:
        raise AssertionError(f'exit status {p.returncode}\n{p.stderr}')
    return p.stderr if stderr else p.stdout

def connect(port, timeout=10):
    deadline = time.time() + timeout
    while True:
        try:
            s = socket.create_connection(('localhost', port), timeout=3)
            return s
        except OSError:
            if time.time() > deadline:
                raise
            time.sleep(0.1)

class server:
    """Runs a server program for the duration of a with statement, and checks
    that SIGTERM ends it."""
    def __init__(self, binary, *args):
        self.cmd = [binary, *args]
    def __enter__(self):
        self.p = subprocess.Popen(self.cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                  text=True)
        return self
    def __exit__(self, *exc):
        self.p.send_signal(signal.SIGTERM)
        try:
            self.p.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.p.kill()
            raise AssertionError('the server did not stop on SIGTERM')
        self.output = self.p.stdout.read()
        return False

def echo_clients(port, n):
    """Opens n connections at once, sends a line on each, and returns how
    many were echoed."""
    socks = [connect(port) for _ in range(n)]
    for i, s in enumerate(socks):
        s.sendall(f'line {i}\n'.encode())
    ok = 0
    for i, s in enumerate(socks):
        try:
            ok += s.recv(100) == f'line {i}\n'.encode()
        except OSError:
            pass
        s.close()
    return ok

def check_hello(binary):
    assert 'Hello, world' in run(binary, '-c1')

def check_shards(binary):
    out = run(binary, '-c4')
    assert 'Seastar is running 4 shards' in out, out
    assert sorted(re.findall(r'Hello from shard (\d+)', out)) == ['0', '1', '2', '3'], out

def check_sleep(binary):
    out = run(binary, '-c1')
    sequential = int(re.search(r'one after the other: (\d+)ms', out).group(1))
    together = int(re.search(r'together: (\d+)ms', out).group(1))
    assert 1900 <= sequential < 2100 and 1000 <= together < 1200, out

def check_echo(binary):
    with server(binary, '-c1') as srv:
        assert echo_clients(1234, 4) == 4
    assert 'shard 0 serves' in srv.output, srv.output

def check_echo_all_cores(binary):
    with server(binary, '-c4') as srv:
        assert echo_clients(1234, 8) == 8
    shards = set(re.findall(r'shard (\d+) serves', srv.output))
    assert shards == {'0', '1', '2', '3'}, srv.output

def check_pong(binary):
    with server(binary, '-c2'):
        s = connect(6380)
        s.sendall(b'PING\r\nPING\r\n')
        reply = b''
        while len(reply) < 14:
            reply += s.recv(100)
        assert reply == b'+PONG\r\n+PONG\r\n', reply

def check_line_server(binary):
    with server(binary, '-c2'):
        s = connect(1235)
        s.sendall(b'hello\nworld\n')
        reply = b''
        while len(reply) < 12:
            reply += s.recv(100)
        assert reply == b'HELLO\nWORLD\n', reply

def check_count_keys(binary):
    out = run(binary, '-c4')
    assert '4 shards hold 406 keys' in out, out   # 100 + 101 + 102 + 103

def check_shoal_v0(binary):
    log = run(binary, '-c2', '--port', '6400', '--logger-log-level', 'shoal=debug', stderr=True)
    assert 'Shoal starting on 2 shards, port 6400' in log, log
    assert 'shard 0 is ready' in log and 'shard 1 is ready' in log, log
    log = run(binary, '-c2', stderr=True)
    assert 'port 6379' in log and 'is ready' not in log, log
    help = run(binary, '--help')
    assert '--port arg (=6379)' in help, help

valkey_checked = set()

def run_tests(version, *args):
    """Runs a Shoal version's tests with its test/run script, which starts the
    server; returns pytest's summary line."""
    p = subprocess.run([str(book / 'shoal' / version / 'test' / 'run'), '-q', *args],
                       capture_output=True, text=True, timeout=600)
    lines = p.stdout.strip().splitlines()
    assert p.returncode == 0, p.stdout[-3000:] + p.stderr[-2000:]
    return next((l for l in reversed(lines) if 'passed' in l), lines[-1] if lines else '')

def check_shoal_with_tests(version, binary):
    """Runs a Shoal version's test suite against it. The first time, also runs
    the suite against valkey-server, if installed, to check the tests."""
    if version not in valkey_checked and shutil.which('valkey-server'):
        valkey_checked.add(version)
        print(f'  tests of shoal/{version} against valkey-server: {run_tests(version, "--valkey")}')
    print(f'  tests of shoal/{version}: {run_tests(version, "--shoal", binary)}')

def check_shoal_v1(binary):
    check_shoal_with_tests('v1', binary)

def check_shoal_v2(binary):
    check_shoal_with_tests('v2', binary)

def check_errors(binary):
    out = run(binary, '-c1', '-m1G')
    lines = [l for l in out.splitlines() if 'ns each' in l]
    assert len(lines) == 4, out
    assert '(0 errors)' in lines[0] and all('(1000000 errors)' in l for l in lines[1:]), out

def check_preemption(binary):
    def experiment(work):
        p = subprocess.run([binary, '-c1', '-m1G', '--work', work],
                           capture_output=True, text=True, timeout=60)
        assert p.returncode == 0, p.stdout + p.stderr
        out = p.stdout + p.stderr
        late = re.search(rf'^{work}: \d+ computations .* the latest ([\d.]+) ms late', out, re.M)
        assert late, out
        return float(late.group(1)), 'Reactor stalled' in out
    # A loop that never yields delays the heartbeat by the whole second, and
    # the stall detector reports it; one that yields when asked doesn't.
    late, stalled = experiment('plain')
    assert late > 900 and stalled, (late, stalled)
    late, stalled = experiment('maybe_yield')
    assert late < 50 and not stalled, (late, stalled)

def check_shoal_v3(binary):
    check_shoal_with_tests('v3', binary)

def check_states(binary):
    out = run(binary, '-c1', '-m1G')
    assert out.splitlines() == [
        'answer_later() starts',
        'answer(): available',
        'no_answer(): failed',
        'answer_later(): not available',
        'answer() gives 42',
        'no_answer() throws: no answer',
        'answer_later() resumes',
        'answer_later() gives 42',
    ], out

def check_promise(binary):
    out = run(binary, '-c1', '-m1G')
    slept = re.search(r'^slept for (\d+)ms$', out, re.M)
    assert slept and 200 <= int(slept.group(1)) < 300, out
    assert 'broken() fails: broken promise' in out, out

def check_shared(binary):
    out = run(binary, '-c1', '-m1G')
    lines = out.splitlines()
    assert sorted(lines[:5]) == ['1 got the value of x', '2 got the value of y',
                                 '3 got the value of x', '4 got the value of y',
                                 '5 got the value of x'], out
    assert lines[4] == '5 got the value of x', out
    loads = re.match(r'^2 loads in (\d+)ms$', lines[5])
    assert loads and 100 <= int(loads.group(1)) < 150, out
    assert lines[6:] == ['starting up', '1 is running', '2 is running', '3 is running'], out

def check_ignored(binary):
    p = subprocess.run([binary, '-c1', '-m1G'], capture_output=True, text=True, timeout=60)
    assert p.returncode == 0, p.stdout + p.stderr
    assert p.stdout.splitlines() == ['dropped the future', 'fail_later() is still running',
                                     'done'], p.stdout
    assert 'Exceptional future ignored: std::runtime_error (nobody is listening)' in p.stderr, \
        p.stderr

def check_cost(binary):
    out = run(binary, '-c1', '-m1G')
    lines = [l for l in out.splitlines() if 'ns per call' in l]
    assert len(lines) == 4 and all('(sum 5000000050000000)' in l for l in lines), out

checks = {
    'states': check_states, 'promise': check_promise, 'shared': check_shared,
    'ignored': check_ignored, 'cost': check_cost,
    'shoal-v3': check_shoal_v3,
    'errors': check_errors,
    'preemption': check_preemption,
    'shoal-v0': check_shoal_v0,
    'shoal-v1': check_shoal_v1,
    'shoal-v2': check_shoal_v2,
    'line-server': check_line_server, 'count-keys': check_count_keys,
    'hello': check_hello, 'shards': check_shards, 'sleep': check_sleep,
    'echo': check_echo, 'echo-all-cores': check_echo_all_cores, 'pong': check_pong,
}

def compile_example(src, std, cxx, flags, out_dir):
    """Compiles one example file, and returns the binary, or None."""
    binary = out_dir / f'{src.stem}-c++{std}'
    p = subprocess.run([cxx, f'-std=c++{std}', '-O2', '-Wall', '-Werror', str(src),
                        '-o', str(binary), *flags], capture_output=True, text=True)
    if p.returncode != 0:
        print(p.stderr)
        return None
    return binary

def build_shoal(src_dir, std, cxx, out_dir):
    """Builds one version of Shoal with CMake, and returns the binary, or None."""
    build_dir = out_dir / f'{src_dir.name}-c++{std}'
    configure = subprocess.run(['cmake', '-S', str(src_dir), '-B', str(build_dir), '-G', 'Ninja',
                                f'-DCMAKE_CXX_COMPILER={cxx}', f'-DCMAKE_CXX_STANDARD={std}',
                                '-DCMAKE_CXX_FLAGS=-Wall -Werror'],
                               capture_output=True, text=True)
    if configure.returncode != 0:
        print(configure.stdout[-3000:], configure.stderr[-3000:])
        return None
    build = subprocess.run(['cmake', '--build', str(build_dir)], capture_output=True, text=True)
    if build.returncode != 0:
        print(build.stdout[-5000:])
        return None
    return build_dir / 'shoal'

def check_toolchain(cxx, stds, out, names):
    """Builds and checks every program with one compiler, against the Seastar
    found through the environment; returns the list of failures."""
    flags = subprocess.run(['pkg-config', '--cflags', '--libs', '--static', 'seastar'],
                           capture_output=True, text=True, check=True).stdout.split()
    flags = [f for f in flags if not f.startswith('-std=')]
    # Each program: its name (for checks and NAMEs), how to show it, and how
    # to build it for a given standard.
    programs = []
    for src in sorted(book.glob('examples/*/*.cc')):
        out_dir = out / src.parent.name
        out_dir.mkdir(parents=True, exist_ok=True)
        programs.append((src.stem, str(src.relative_to(book)),
                         lambda std, src=src, out_dir=out_dir:
                             compile_example(src, std, cxx, flags, out_dir)))
    for src_dir in sorted(book.glob('shoal/v*/'), key=shoal_version):
        (out / 'shoal').mkdir(parents=True, exist_ok=True)
        programs.append((f'shoal-{src_dir.name}', str(src_dir.relative_to(book)),
                         lambda std, src_dir=src_dir:
                             build_shoal(src_dir, std, cxx, out / 'shoal')))
    if names:
        programs = [p for p in programs if p[0] in names]
    failed = []
    for name, shown, build in programs:
        binaries = {}
        for std in stds:
            binaries[std] = build(std)
            status = 'built' if binaries[std] else 'FAILED to build'
            print(f'{shown} with C++{std} ({cxx}): {status}')
            if not binaries[std]:
                failed.append(f'{name} C++{std} {cxx}')
        if name not in checks:
            print(f'{shown}: no run check')
            continue
        if not binaries[stds[0]]:
            continue
        try:
            checks[name](str(binaries[stds[0]]))
            print(f'{shown} ({cxx}): runs correctly')
        except Exception as e:
            print(f'{shown} ({cxx}): run FAILED: {e}')
            failed.append(f'{name} run {cxx}')
    return failed

# Every Shoal version is complete, so each has its own copy of the test
# tools. They should be the same in all versions, except where a version
# changed them on purpose: list those here, as (version, file name).
INTENDED_TEST_TOOL_CHANGES = set()

def shoal_version(src_dir):
    """The version number of a Shoal directory, such as (2,) for v2, or (1, 1) for v1.1."""
    return tuple(int(n) for n in src_dir.name[1:].split('.'))

def check_test_tool_copies():
    """Checks that each Shoal version's copies of the test tools are the same
    as the previous version's; returns the list of failures."""
    failed = []
    versions = sorted((d for d in book.glob('shoal/v*/') if (d / 'test').is_dir()),
                      key=shoal_version)
    for previous, current in zip(versions, versions[1:]):
        for name in ('run', 'conftest.py'):
            a, b = previous / 'test' / name, current / 'test' / name
            if (current.name, name) in INTENDED_TEST_TOOL_CHANGES:
                continue
            if not b.exists() or a.read_bytes() != b.read_bytes():
                print(f'{b.relative_to(book)} differs from {a.relative_to(book)}; '
                      f'fix both, or list it in INTENDED_TEST_TOOL_CHANGES')
                failed.append(f'{current.name} {name}')
    return failed

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--seastar', action='append', default=[],
                        metavar='CXX[:STD]=PREFIX[:PREFIX...]',
                        help='a compiler and the Seastar built with it (repeatable)')
    parser.add_argument('--std', default='23,26', help='comma-separated C++ standards')
    parser.add_argument('--out', type=Path, default=book / 'build' / 'examples')
    parser.add_argument('names', nargs='*')
    args = parser.parse_args()
    stds = args.std.split(',')
    failed = check_test_tool_copies()
    if not args.seastar:
        failed += check_toolchain("g++", stds, args.out / "g++", args.names)
    for spec in args.seastar:
        compiler, prefixes = spec.split('=', 1)
        cxx, _, std = compiler.partition(':')
        prefixes = [str(Path(p).expanduser()) for p in prefixes.split(':')]
        os.environ['CMAKE_PREFIX_PATH'] = ':'.join(prefixes)
        os.environ['PKG_CONFIG_PATH'] = ':'.join(
            f'{p}/{lib}/pkgconfig' for p in prefixes for lib in ('lib64', 'lib'))
        failed += check_toolchain(cxx, [std] if std else stds,
                                  args.out / (f'{cxx}-c++{std}' if std else cxx), args.names)
    if failed:
        sys.exit('failed: ' + ', '.join(failed))
    print('all checks passed')

main()
