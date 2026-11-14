#!/usr/bin/python3 -I
# Build and run the book's example programs (examples/*/*.cc) and every
# version of Shoal (shoal/v*/), to check that the code the book shows really
# compiles and works.
#
# Usage: check_examples.py [--seastar CXX[:STD]=PREFIX[:PREFIX...]]...
#                          [--out DIR] [NAME...]
#
# Each --seastar names a compiler and the Seastar built with it: the
# installation prefix, followed by any other prefixes its dependencies need
# (for cooked dependencies, the build directory's _cooking/installed), and,
# with :STD, the C++ standard that this Seastar was built for.
#
# Every program is built as the book tells readers to build it, without
# choosing a C++ standard: the examples with the compiler and pkg-config's
# flags, which include Seastar's standard, and Shoal with CMake and
# find_package(Seastar), which raises the program's standard to Seastar's.
# With :STD, the check also verifies that each program was compiled for
# that standard. The book promises that its code works with both GCC and
# Clang, in C++23 and C++26, so the complete check uses four Seastars:
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
# Then the programs are run: those that end by themselves are run and their
# output checked, and the servers are started, tested with a client, and
# killed. NAMEs limit the check to the examples with these file names
# (without .cc), to Shoal versions (shoal-v0, ...), or to solutions that
# change Shoal, by their patches' names (shoal-scan, ...).

import argparse
import os
import shutil
import re
import signal
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

book = Path(__file__).resolve().parent.parent
# Run with python3 -I, which leaves this directory out of the module path.
sys.path.insert(0, str(Path(__file__).resolve().parent))
import shoal_patch  # noqa: E402

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

def check_count_bytes(binary):
    with server(binary, '-c1'):
        socks = [connect(1234) for _ in range(3)]
        for i, s in enumerate(socks):
            s.sendall(b'x' * (1000 * i + 5))
            s.shutdown(socket.SHUT_WR)
        for i, s in enumerate(socks):
            reply = b''
            while chunk := s.recv(100):
                reply += chunk
            assert reply == f'{1000 * i + 5} bytes\n'.encode(), reply
            s.close()

def check_fiasco(binary):
    # The wrong variant reads freed memory, so its output is unpredictable;
    # only the two correct ones are checked.
    for variant in ('right', 'old'):
        out = run(binary, '-c1', '-m1G', '--variant', variant)
        assert out == 'hello, the reader of this book\n', out

def check_sharing(binary):
    out = run(binary, '-c1', '-m1G')
    assert out.splitlines() == [
        's1: still here', 's1: still here', 's1: still here',
        'owner let go; the session still exists',
        's1: keepalive done', 's1: destroyed',
        'later, the session is gone'], out

def check_closing(binary):
    out = run(binary, '-c1', '-m1G')
    assert out.splitlines() == [
        'two pointers to segment 1', 'segment 1: unused, closing',
        'no pointers left; shutting down', 'segment 1: closed',
        'segment 1: deleted', 'done'], out

def percentages(line):
    """The group names and percentages in a line of cpu-shares.cc."""
    return {name: int(pct) for name, pct in re.findall(r'(\w+) (\d+)%', line)}

def check_shares(out, expected):
    lines = out.splitlines()
    assert len(lines) == len(expected), out
    for line, want in zip(lines, expected):
        got = percentages(line.split(':', 1)[1])
        assert got.keys() == want.keys(), out
        assert all(abs(got[k] - want[k]) <= 3 for k in want), out

def check_fibers(binary):
    out = run(binary, '-c1', '-m1G')
    m = re.fullmatch(r'one group: one loop (\d+)%, ten loops (\d+)%\n'
                     r'two groups: one loop (\d+)%, ten loops (\d+)%\n', out)
    assert m and 6 <= int(m[1]) <= 12 and 45 <= int(m[3]) <= 55, out

def check_cpu_shares(binary):
    check_shares(run(binary, '-c1', '-m1G'),
                 [{'high': 57, 'medium': 29, 'low': 14}, {'medium': 67, 'low': 33}])

def check_scheduling_groups_ex1(binary):
    check_shares(run(binary, '-c1', '-m1G'),
                 [{'high': 50, 'medium': 33, 'low': 17}, {'medium': 67, 'low': 33}])

def check_which_group(binary):
    out = run(binary, '--smp', '2', '-m1G')
    groups = [line.split()[-1] for line in out.splitlines()]
    assert groups == ['main', 'background', 'background', 'background', 'background',
                      'main', 'background', 'main', 'background', 'main', 'background'], out

def check_scheduling_groups_ex2(binary):
    out = run(binary, '-c1', '-m1G')
    assert [line.split()[-1] for line in out.splitlines()] == [
        'background', 'main', 'background'], out

def check_group_data(binary):
    out = run(binary, '-c1', '-m1G')
    assert out == 'interactive: 3 requests\nbatch: 2 requests\n', out

def check_order(binary):
    out = run(binary)
    assert re.fullmatch(r'got 100 bytes, told (100|0)\n', out), out

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
    """Runs a Shoal version's tests, and those of the earlier versions, with
    shoal/test/run, which starts the server; returns pytest's summary line.
    version is a directory of shoal/, such as "v4"."""
    p = subprocess.run([str(book / 'shoal' / 'test' / 'run'), version, '-q', *args],
                       capture_output=True, text=True, timeout=600)
    lines = p.stdout.strip().splitlines()
    assert p.returncode == 0, p.stdout[-3000:] + p.stderr[-2000:]
    return next((l for l in reversed(lines) if 'passed' in l), lines[-1] if lines else '')

def check_shoal_with_tests(version, binary, name=None, extra=()):
    """Runs a Shoal version's test suite against it (with extra, more
    arguments for shoal/test/run, for a solution's own tests and server
    options). The first time, also runs the suite against valkey-server, if
    installed, to check the tests."""
    name = name or f'shoal/{version}'
    if name not in valkey_checked and shutil.which('valkey-server'):
        valkey_checked.add(name)
        print(f'  tests of {name} against valkey-server: '
              f'{run_tests(version, "--valkey", *extra)}')
    print(f'  tests of {name}: {run_tests(version, "--shoal", binary, *extra)}')

def check_solution(patch, src_dir, binary):
    """Runs the tests of the Shoal version that a solution's patch changes,
    and the solution's own tests, if it has any, against the changed Shoal,
    with the server options that the patch's header asks for."""
    base, options = shoal_patch.header(patch)
    extra = []
    if (src_dir / 'test_solutions.py').exists():
        extra += ['--test', str(src_dir / 'test_solutions.py')]
    for option in options:
        extra += [f'--server-option={option}']
    check_unit_tests(binary, patch.stem)
    check_shoal_with_tests(base.name, binary, patch.stem, extra)

def check_shoal_v1(binary):
    check_shoal_with_tests('v1', binary)

def check_shoal_v2(binary):
    check_shoal_with_tests('v2', binary)

def check_errors(binary):
    out = run(binary, '-c1', '-m1G')
    lines = [l for l in out.splitlines() if 'ns each' in l]
    # With C++26, there is an eighth line, for std::exception_ptr_cast.
    cxx26 = 'exception_ptr_cast' in out
    assert len(lines) == (8 if cxx26 else 7), out
    assert '(0 errors)' in lines[0] and all('(1000000 errors)' in l for l in lines[1:]), out
    assert ('other errors pass through' in out) == cxx26, out

def check_preemption(binary):
    def experiment(work):
        p = subprocess.run([binary, '-c1', '-m1G', '--work', work],
                           capture_output=True, text=True, timeout=60)
        assert p.returncode == 0, p.stdout + p.stderr
        out = p.stdout + p.stderr
        late = re.search(rf'^{work}: \d+ computations .* the most delayed ([\d.]+) ms late', out, re.M)
        assert late, out
        return float(late.group(1)), 'Reactor stalled' in out
    # A loop that never yields delays the heartbeat by the whole second, and
    # the stall detector reports it; one that yields when asked doesn't.
    late, stalled = experiment('plain')
    assert late > 900 and stalled, (late, stalled)
    late, stalled = experiment('maybe_yield')
    assert late < 50 and not stalled, (late, stalled)

def check_count_lines(binary):
    out = run(binary, '-c1', '-m1G')
    m = re.fullmatch(r'98802 lines contain "42"; meanwhile, another task ran (\d+) times\n', out)
    # The count takes milliseconds, so it yields a few times.
    assert m and int(m.group(1)) > 1, out

def check_shoal_v3(binary):
    check_shoal_with_tests('v3', binary)

def check_shoal_v4(binary):
    check_shoal_with_tests('v4', binary)

def check_shoal_v5(binary):
    check_shoal_with_tests('v5', binary)

def check_shoal_v6(binary):
    check_shoal_with_tests('v6', binary)

def check_shoal_v7(binary):
    check_shoal_with_tests('v7', binary)

def check_shoal_v8(binary):
    check_shoal_with_tests('v8', binary)

def check_shoal_v9(binary):
    check_shoal_with_tests('v9', binary)

def check_shoal_v10(binary):
    check_shoal_with_tests('v10', binary)

def check_shoal_v11(binary):
    check_shoal_with_tests('v11', binary)

def check_shoal_v12(binary):
    check_shoal_with_tests('v12', binary)

def check_shoal_v13(binary):
    check_shoal_with_tests('v13', binary)

def check_shoal_v14(binary):
    check_shoal_with_tests('v14', binary)

def check_shoal_v15(binary):
    check_shoal_with_tests('v15', binary)

def check_unit_tests(binary, name):
    """Runs the unit tests of a Shoal (since version 16), which are a
    program of their own, built next to the server, if it has them."""
    unit_tests = Path(binary).parent / 'shoal_unit_tests'
    if not unit_tests.exists():
        return
    out = subprocess.run([str(unit_tests), '--', '-c2', '-m1G'],
                         capture_output=True, text=True, timeout=300)
    assert out.returncode == 0 and 'No errors detected' in out.stdout + out.stderr, \
        out.stdout + out.stderr
    print(f'  unit tests of {name}: no errors')
    # And its benchmarks, briefly: each test in perf_tests.cc must report.
    perf_tests = Path(binary).parent / 'shoal_perf_tests'
    out = subprocess.run([str(perf_tests), '-c1', '-m1G', '--blocked-reactor-notify-ms',
                          '2000', '--duration', '0.1', '--runs', '1'],
                         capture_output=True, text=True, timeout=300)
    cache = (Path(binary).parent / 'CMakeCache.txt').read_text()
    src_dir = re.search(r'^CMAKE_HOME_DIRECTORY:INTERNAL=(.*)$', cache, re.M)[1]
    source = (Path(src_dir) / 'perf_tests.cc').read_text()
    tests = [f'{group}.{test}' for group, test in
             re.findall(r'^PERF_TEST(?:_F)?\((\w+), (\w+)\)', source, re.M)]
    reported = re.findall(r'^(\S+)\s+\d+\s+[\d.]+[nµm]?s ', out.stdout, re.M)
    assert out.returncode == 0 and tests and reported == tests, out.stdout + out.stderr
    print(f'  benchmarks of {name}: {len(tests)} reported')

def check_shoal_v16(binary):
    check_unit_tests(binary, 'shoal/v16')
    # And chapter 29's repeat test, which finds leaks of reachable memory.
    check_shoal_with_tests('v16', binary, extra=(
        '--test', str(book / 'examples/memory-leaks/test_memory.py')))

def check_testing_ex2(binary):
    # In a debug build, the wrong version finds no greeting in about half
    # of its 100 tries; in release, never.
    assert run(binary, '-c1', '-m1G') == ('wrong: no greeting in 0 of 100\n'
                                          'right: no greeting in 0 of 100\n')

def check_table_fuzz(binary):
    assert run(binary) == '200 random inputs: no differences\n'

def check_verbs(binary):
    out = run(binary, '-c1', '-m1G')
    assert out == ('1 + 2 + 3 = 6\n'
                   "an older client's 1 + 2 = 3\n"
                   'note from 127.0.0.1: hello\n'
                   "fail: remote_verb_error: std::runtime_error (no key 'x')\n"
                   'wait: timeout_error: rpc call timed out\n'
                   'stats: unknown_verb_error: verb 5\n'
                   'after the server stopped: closed_error: connection is closed\n'), out

def check_retry(binary):
    out = run(binary, '-c1', '-m1G')
    assert out == ('busy; retrying after 10 ms\nbusy; retrying after 20 ms\n'
                   '200: ready\n'), out

def check_http_ex4(binary):
    out = run(binary, '-c1', '-m1G')
    assert out == ('default pool: 0 keys left, 100 connections opened\n'
                   'pool of 10: 0 keys left, 10 connections opened\n'), out

def check_allocations(binary):
    out = run(binary, '-c1', '-m1G')
    for line in ['600 KB took 1024 KB', '900 MB: std::bad_alloc',
                 '600 MB: std::bad_alloc', '500 MB: allocated']:
        assert line in out, out

def check_cross_free(binary):
    out = run(binary, '--smp', '4', '-m4G')
    assert re.fullmatch(r'freed by shard 0, their owner: [\d.]+ ns each\n'
                        r'(freed by \d other shards? at once: [\d.]+ ns each\n){3}', out), out

def check_reclaimer(binary):
    out = run(binary, '-c1', '-m1G')
    m = re.fullmatch(r'(\d+) blobs kept, (\d+) dropped, \d+ MB free\n', out)
    assert m and int(m[1]) + int(m[2]) == 3000 and int(m[2]) > 1000, out

def check_memory_ex2(binary):
    out = run(binary, '-c1', '-m1G')
    m = re.search(r'every other block: (\d+) MB free, largest allocation (\d+) KB', out)
    assert m and 450 < int(m[1]) < 550 and int(m[2]) <= 8192, out

def check_basics(binary):
    out = run(binary, '-c1', '-m1G')
    assert re.match(r'the answer, 42, took 1\d\d ms\n'
                    r'(round \d: a thread \d+ ns, a coroutine \d+ ns\n){3}$', out), out

def check_deferred(binary):
    out = run(binary, '-c1', '-m1G')
    once = 'database started\nserver started\nserver stopped\ndatabase stopped\n'
    assert out == once + once + 'run: something failed\n', out

def check_long_work(binary):
    out = run(binary, '-c1', '-m1G', timeout=120)
    m = re.fullmatch(r'not yielding: 348513 primes in \d+ ms; the heartbeat was up to '
                     r'([\d.]+) ms late\nyielding: 348513 primes in \d+ ms; the heartbeat '
                     r'was up to ([\d.]+) ms late\n', out)
    assert m and float(m[1]) > 300 and float(m[2]) < 50, out

def check_alien(binary):
    out = run(binary, '-c2', '-m1G')
    assert out == "the shards' answers add up to 100\n", out

def check_worker(binary):
    out = run(binary, '-c1', '-m1G', timeout=120)
    m = re.fullmatch(r'in the worker: got 42 and 348513 primes, and the timer fired (\d+) times\n'
                     r'on the shard: got 42 and 348513 primes, and the timer fired (\d+) times\n', out)
    assert m and int(m[1]) > 300 and int(m[2]) < 10, out

def check_process(binary):
    out = run(binary, '-c1', '-m1G')
    assert out == 'pear\nfig\napple\nsort exited with status 0\n', out

def check_threads_ex1(binary):
    out = run(binary, '-c1', '-m1G')
    m = re.fullmatch(r'sum 4950; at most (\d+) functions waited; \d+ ms\n', out)
    assert m and int(m[1]) <= 16, out

def check_threads_ex2(binary):
    out = run(binary, '-c1', '-m1G')
    m = re.fullmatch(r'4 workers: in order: true; (\d+) ms\n1 workers: in order: true; (\d+) ms\n', out)
    assert m and int(m[1]) < 800 and int(m[2]) >= 1600, out

def check_threads_ex3(binary):
    out = run(binary, '-c2', '-m1G')
    assert re.fullmatch(r'sending: \d+ ns each; until all ran: \d+ ns\n', out), out

def check_threads_ex4(binary):
    check_deferred(binary)

def check_threads_ex5(binary):
    out = run(binary, '-c1', '-m1G')
    assert out == ('exited with status 3\nkilled by signal 9\n'
                   'failed to start: No such file or directory\n'), out

def check_sources(binary):
    out = run(binary, '-c1', '-m4G', timeout=120)
    names = ['destroying a vector of a million strings',
             'destroying an unordered_map of a million keys', 'sorting ten million numbers',
             'copying them (40 MB)', 'formatting them into a log message',
             'throwing and catching 100,000 exceptions', 'writing to 512 MB of fresh memory',
             'writing to the same 512 MB again']
    lines = out.splitlines()
    assert len(lines) == len(names), out
    for line, name in zip(lines, names):
        assert re.fullmatch(re.escape(name) + r' +[\d.]+ ms', line), out

def check_gentle(binary):
    out = run(binary, '-c1', '-m4G', timeout=120)
    m = re.fullmatch(r'at once: the heartbeat was up to ([\d.]+) ms late\n'
                     r'gradually, in \d+ ms: the heartbeat was up to ([\d.]+) ms late\n', out)
    assert m and float(m[1]) > 50 and float(m[2]) < 20, out

def check_threshold(binary):
    err = run(binary, '-c1', '-m1G', stderr=True)
    parts = err.split('threshold 5 ms: working for 15 ms')
    assert len(parts) == 2 and 'stalled' not in parts[0] and 'stalled' in parts[1], err

def check_stalls_ex2(binary):
    out = run(binary, '-c1', '-m1G')
    assert re.fullmatch(r'inserted in order: destroyed in [\d.]+ ms\n'
                        r'inserted in a random order: destroyed in [\d.]+ ms\n', out), out

def check_stalls_ex3(binary):
    out = run(binary, '-c1', '-m4G', timeout=120)
    m = re.fullmatch(r'at once: \d+ ms; the heartbeat was up to ([\d.]+) ms late\n'
                     r'gently: \d+ ms; the heartbeat was up to ([\d.]+) ms late\n', out)
    assert m and float(m[1]) > 20 and float(m[2]) < 20, out

def check_stalls_ex4(binary):
    err = run(binary, '-c1', '-m1G', stderr=True)
    assert all(f'working for {ms} ms' in err for ms in range(4, 15)), err

def check_stalls_ex5(binary):
    out = run(binary, '-c1', '-m1G', timeout=120)
    m = re.fullmatch(r'at once: \d+ ms, sorted: true; the heartbeat was up to ([\d.]+) ms late\n'
                     r'in pieces: \d+ ms, sorted: true; the heartbeat was up to ([\d.]+) ms late\n',
                     out)
    assert m and float(m[1]) > 100 and float(m[2]) < 20, out

def check_errors_then(binary):
    out = run(binary, '-c1', '-m1G')
    assert out == ('"21": 42\n"21": done\n"abc": not a number (stoi)\n"abc": done\n'
                   'show("") threw: empty\nfuturize_invoke: a failed future: empty\n'), out

def check_when_all(binary):
    out = run(binary, '-c1', '-m1G')
    assert out == 'when_all_succeed: 1 + 2 = 3\nwhen_all: 3, and a failure\n', out

def check_cost_then(binary):
    out = run(binary, '-c1', '-m1G', timeout=120)
    assert len(re.findall(r' [\d.]+ ns per call', out)) == 3, out

def check_continuations_ex2(binary):
    out = run(binary, '-c1', '-m1G')
    assert out == ('"21": 42\n"21": done\n"abc": not a number (stoi)\n"abc": done\n'
                   '"": not a number (empty)\n"": done\n'), out

def check_continuations_ex3(binary):
    out = run(binary, '-c1', '-m1G', timeout=120)
    m = re.fullmatch(r'recursion, 100,000: [\d.]+ ms; the heartbeat was up to [\d.]+ ms late\n'
                     r'repeat\(\), 10,000,000: [\d.]+ ms; the heartbeat was up to ([\d.]+) ms late\n',
                     out)
    assert m and float(m[1]) < 20, out

def check_run_order(binary):
    out = run(binary, '-c1', '-m1G')
    lines = out.splitlines()
    assert lines[:5] == ['caller: calls the callee', '  callee: starts, inside its caller',
                         'caller: got its future, available: false',
                         '  callee: resumes, as a task of its own',
                         'caller: the callee is done'], out
    small = int(re.search(r'frame of small\(\): (\d+) bytes', out)[1])
    large = int(re.search(r'frame of large\(\): (\d+) bytes', out)[1])
    assert 64 <= small <= 512 and large >= small + 1000, out

def check_preempt(binary):
    out = run(binary, '-c1', '-m1G', timeout=120)
    m = re.fullmatch(r'co_await: sum 25000000 in \d+ ms; the heartbeat was up to ([\d.]+) ms late\n'
                     r'without_preemption_check: sum 25000000 in \d+ ms; the heartbeat was up to '
                     r'([\d.]+) ms late\n', out)
    assert m and float(m[1]) < 20 and float(m[2]) > 30, out

def check_awaitable(binary):
    out = run(binary, '-c1', '-m1G')
    m = re.fullmatch(r'sleep_for\(10ms\) slept ([\d.]+) ms\n'
                     r'100 sleeps: sleep_for\(\) allocated 0 times, seastar::sleep\(\) (\d+)\n', out)
    assert m and float(m[1]) >= 10 and int(m[2]) > 0, out

def check_generators(binary):
    out = run(binary, '-c1', '-m1G')
    want = ([f'count_down: {i}' for i in (3, 2, 1)] + [f'squares: {i * i}' for i in range(1, 7)]
            + [f'evens: {i}' for i in (0, 2, 4, 6)])
    assert out.splitlines() == want, out

def check_how_coroutines_work_ex1(binary):
    out = run(binary, '-c1', '-m1G')
    assert re.fullmatch(r'slept after 1\d ms\naborted after [5-9] ms\n', out), out

def check_how_coroutines_work_ex2(binary):
    out = run(binary, '-c1', '-m1G')
    assert re.search(r'outer\(\), awaiting inner\(\): 2 allocations', out), out

def check_how_coroutines_work_ex3(binary):
    out = run(binary, '-c1', '-m1G', timeout=120)
    assert len(re.findall(r'[\d.]+ ns per value', out)) == 2, out

def check_how_coroutines_work_ex4(binary):
    err = run(binary, '-c2', '-m1G', stderr=True)
    assert 'caller: after hop(), on shard 1' in err, err

def check_how_coroutines_work_ex5(binary):
    out = run(binary, '-c1', '-m1G')
    assert out == '33334 keys start with "item:"\n', out

def check_memory_ex3(binary):
    for args in [(), ('random',)]:
        out = run(binary, '-c1', '-m1G', *args)
        m = re.fullmatch(r'(oldest|random): kept \d+ \((\d+) MB\), dropped \d+, '
                         r'\d+ calls, \d+ MB free\n', out)
        assert m and int(m[2]) > 900, out

def check_udp(binary):
    out = run(binary, '-c1', '-m1G')
    assert re.fullmatch(r'sent hello, received HELLO from 127\.0\.0\.1:\d+\n'
                        r'sent datagram, received DATAGRAM from 127\.0\.0\.1:\d+\n', out), out

def check_random_reads(binary):
    # A small file, in the current directory, which the program removes.
    out = run(binary, '-c1', '-m1G', '--size', '16', '--reads', '200',
              '--file', 'random-reads-check.dat')
    for n in (1, 4, 16, 64):
        assert re.search(rf'^ *{n} in flight: +\d+ reads a second', out, re.M), out

def check_reads_and_writes(binary):
    # Small files, in the current directory, which the program removes.
    out = run(binary, '-c1', '-m1G', '--size', '16', '--seconds', '1')
    reads = r'  reads: \d+ a second, [\d.]+ ms on average, [\d.]+ ms for the slowest 1%\n'
    assert re.fullmatch(f'alone:\n{reads}with the writes:\n{reads}'
                        r'  writes: \d+ MB a second\n', out), out

def check_cancel(binary):
    out = run(binary, '-c1', '-m1G')
    m = re.fullmatch(r'100 reads: (\d+) done, (\d+) cancelled\n', out)
    assert m and int(m.group(1)) + int(m.group(2)) == 100, out

def check_withdraw(binary):
    out = run(binary, '-c1', '-m1G')
    assert out == ('withdraw: 2 of 2 withdrawals done, balance -60\n'
                   'withdraw_safely: 1 of 2 withdrawals done, balance 20\n'), out

def check_units(binary):
    out = run(binary, '-c1', '-m1G')
    assert out == ('10 downloads, at most 3 at a time\n'
                   '40 left, 2 requests waiting\n'
                   '40 left, 0 requests waiting\n'
                   'Semaphore timed out: memory\n'), out

def check_deadlock(binary):
    out = run(binary, '-c1', '-m1G')
    m = re.fullmatch(r'update_index: Semaphore timedout\n'
                     r'meanwhile, the heartbeat beat (\d+) times\n', out)
    # The shard kept running while update_table() waited.
    assert m and int(m.group(1)) >= 10, out

def check_semaphore_cost(binary):
    out = run(binary, '-c1', '-m1G')
    for what in ('get_units(), free', 'try_get_units(), free', 'std::mutex, free',
                 'a unit passed between two fibers, per turn'):
        assert re.search(rf'^{re.escape(what)}: [0-9.]+ ns$', out, re.M), out

def check_condition(binary):
    out = run(binary, '-c1', '-m1G')
    assert out == ('wait() returned at once\n'
                   'wait(): took 1 first, and the waiter waited for 2\n'
                   'when(): took 1 first, and the waiter got -1\n'), out

def check_gate(binary):
    out = run(binary, '-c1', '-m1G')
    assert out == ('3 saves in the background\nsaved file 1\nsaved file 2\n'
                   'saved file 3\nclosed, 0 in the background\n'
                   'too late: gate closed\n'), out

def check_queue(binary):
    out = run(binary, '-c1', '-m1G')
    expected = ''.join(f'consumed {i}\n' for i in range(1, 11))
    assert out == expected + 'no more items; at most 3 waited\n', out

def check_rwlock(binary):
    out = run(binary, '-c1', '-m1G')
    assert out == ('reader 1 reads\nreader 2 reads\nthe writer writes\n'
                   'reader 3 reads\n'), out

def check_clocks(binary):
    out = run(binary, '-c1', '-m1G')
    for clock in ('std::chrono::steady_clock', 'std::chrono::system_clock',
                  'lowres_clock', 'lowres_system_clock'):
        assert re.search(rf'^{re.escape(clock)} +[0-9.]+ ns$', out, re.M), out
    # lowres_clock isn't updated while a task runs without yielding.
    assert 'in 5 ms of work, lowres_clock moved 0 ms' in out, out

def check_timers(binary):
    out = run(binary, '-c1', '-m1G')
    assert out.splitlines() == ['tick 1', 'tick 2', 'tick 3',
                                'stopping the ticker; armed: true',
                                '3 ticks; armed: false'], out

def check_timeouts(binary):
    out = run(binary, '-c1', '-m1G')
    assert out.splitlines() == ['timed out', 'slow_operation() finished', '--',
                                'abort requested', 'aborted', '--',
                                'stopped: timedout'], out

def check_manual(binary):
    out = run(binary, '-c1', '-m1G')
    assert out.splitlines() == ['after 9 s: false', 'after 10 s: true'], out

def check_pubsub_bench(binary):
    """Runs pubsub-bench against a valkey-server, with 20 subscribers and two
    publishers of 500 messages each, and checks that every subscriber
    received all 1000."""
    if not shutil.which('valkey-server'):
        print('  (no valkey-server: pubsub-bench not run)')
        return
    with server('valkey-server', '--port', '6441', '--save', ''):
        connect(6441).close()
        out = run(binary, '-c1', '-m512M', '--port', '6441', '--subscribers', '20',
                  '--messages', '500', '--publishers', '2')
    assert 'received 20000 messages of 100 bytes from 2 publishers' in out, out

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

def check_throwing(binary):
    out = run(binary, '-c1', '-m1G')
    good = ['stored "a"', 'couldn\'t store ""', 'stored "c"']
    assert out.splitlines() == [
        'with store_all_bad():', 'stored "a"', 'store_all_bad() threw: empty key',
        'with store_all_good():', *good,
        'with store_carefully():', *good,
    ], out

def check_kinds(binary):
    out = run(binary, '-c1', '-m512M')
    assert out.splitlines() == [
        'semaphore wait: a timeout: seastar::semaphore_timed_out (Semaphore timedout)',
        'condition wait: something else: seastar::condition_variable_timed_out (Condition variable timed out)',
        'sleep: an abort: seastar::sleep_aborted (Sleep is aborted)',
        'semaphore wait: an abort: seastar::semaphore_aborted (Semaphore aborted)',
        'gate: something else: seastar::gate_closed_exception (gate closed)',
        'promise: something else: seastar::broken_promise (broken promise)',
        'seastar::nested_exception: std::runtime_error (disk error) '
        '(while cleaning up after std::runtime_error (network error))',
    ], out

def check_internal(binary):
    p = subprocess.run([binary, '-c1', '-m512M'], capture_output=True, text=True, timeout=60)
    assert p.returncode == 0, p.stdout + p.stderr
    assert p.stdout.splitlines() == ['the withdrawal failed, and the program goes on',
                                     'the deposit failed'], p.stdout
    assert 'app - negative balance -50 after withdrawing 150, at: 0x' in p.stderr, p.stderr
    assert 'app - check failed: n > 0, at: 0x' in p.stderr, p.stderr

def check_expected(binary):
    out = run(binary, '-c1', '-m512M')
    assert out.splitlines() == ['x is 42', "y isn't a number", "z wasn't found"], out

def check_overload(binary):
    out = run(binary, '-c1', '-m512M', timeout=120)
    lines = out.splitlines()
    assert len(lines) == 12, out
    served = {}
    for l in lines:
        m = re.match(r'^ *(\d+) clients, (with|without) exceptions *: *(\d+) served/s, *(\d+) timed out/s$', l)
        assert m, l
        served[(int(m.group(1)), m.group(2))] = int(m.group(3))
    # The overloaded server serves far more without exceptions.
    assert served[(1000, 'without')] > 10 * served[(1000, 'with')], out

def check_cost(binary):
    out = run(binary, '-c1', '-m1G')
    lines = [l for l in out.splitlines() if 'ns per call' in l]
    assert len(lines) == 4 and all('(sum 5000000050000000)' in l for l in lines), out

def check_periodic(binary):
    out = run(binary, '-c1', '-m512M')
    m = re.fullmatch(r'sleep\(\): stopping took ([0-9.]+) seconds\n'
                     r'sleep_abortable\(\): stopping took ([0-9.]+) seconds\n'
                     r'condition_variable: stopping took ([0-9.]+) seconds\n', out)
    # The first waits out its second-long sleep; the others stop at once.
    assert (m and float(m.group(1)) >= 0.8 and float(m.group(2)) <= 0.1
            and float(m.group(3)) <= 0.1), out

def check_deploying_and_tuning_ex1(binary):
    # With linux-aio: with io_uring, a Seastar bug makes it hang (the
    # reactor doesn't notice timers while it runs an idle handler).
    out = run(binary, '-c1', '-m1G', '--reactor-backend', 'linux-aio')
    lines = out.splitlines()
    assert len(lines) == 3 and lines[0].startswith('no idle handler: heartbeat up to'), out
    late = [float(x) for x in re.findall(r'up to ([\d.]+) ms late', out)]
    assert late[1] < 5 and late[2] > 50, out

def check_deploying_and_tuning_ex2(binary):
    out = run(binary, '-c1', '-m1G')
    busy = float(re.search(r'^ +busy: +([\d.]+) ms$', out, re.M)[1])
    assert len(out.splitlines()) == 8 and 800 < busy < 1200, out

def check_idioms(binary):
    assert run(binary, '-c1', '-m512M') == (
        'retry: attempt 1 failed\n'
        'retry: attempt 2 failed\n'
        'retry: attempt 3 failed\n'
        'retry: attempt 4 succeeded\n'
        'coalescing: 10 triggers, 2 runs\n'
        'barrier: ["a ended", "barrier passed", "b ended"]\n')

def check_lessons_from_large_code_bases_ex1(binary):
    # The delays are random, so the attempts and time vary; the retry must
    # give up before its deadline, 200 ms away.
    out = run(binary, '-c1', '-m512M')
    m = re.fullmatch(r'succeeded at attempt 3\n'
                     r'gave up after (\d+) attempts, (\d+) ms: never\n', out)
    assert m and int(m.group(1)) >= 3 and int(m.group(2)) < 200, out

def check_lessons_from_large_code_bases_ex2(binary):
    for shards in (2, 4):
        out = run(binary, f'-c{shards}', '-m2G')
        lines = out.splitlines()
        assert (len(lines) == 2 and lines[0].endswith(': a deadlock')
                and lines[1] == f'shard 0 first: a got {shards} of {shards} locks, '
                                f'b got {shards} of {shards}'), out

def check_cycle(binary):
    # A release build doesn't detect the cycle (a debug build's LeakSanitizer
    # reports the session as a direct leak, and its buffer as indirect).
    assert run(binary, '-c1', '-m512M') == 'main is done\n'

def live_objects_by_round(out):
    return [int(n) for n in re.findall(r'^after round \d+: \d+ KB allocated, (\d+) live objects$',
                                       out, re.M)]

def check_stuck(binary):
    # Every round leaves 100 stuck requests, of five objects each (four
    # when built by Clang).
    out = run(binary, '-c1', '-m1G')
    live = live_objects_by_round(out)
    growth = [b - a for a, b in zip(live[1:], live[2:])]
    assert len(live) == 5 and all(395 <= g <= 405 or 495 <= g <= 505 for g in growth), out

def check_memory_leaks_ex2(binary):
    assert run(binary, '-c1', '-m512M') == 'main is done\n'

def check_memory_leaks_ex3(binary):
    # With a timeout, the stuck requests end, and the live objects stop
    # growing: the last four counts (rounds 25 to 40) are all the same.
    out = run(binary, '-c1', '-m1G')
    live = live_objects_by_round(out)
    assert len(live) == 8 and max(live[4:]) - min(live[4:]) <= 10, out

def check_abandon(binary):
    # A release build doesn't detect the leak (a debug build's LeakSanitizer
    # does, and exits with status 1).
    assert run(binary, '-c1', '-m512M') == 'main is done\n'

def check_owns(binary):
    out = run(binary, '-c2', '-m2G')
    lines = sorted(out.splitlines())
    assert len(lines) == 2, out
    for i, line in enumerate(lines):
        assert re.fullmatch(rf'shard {i} of 2: on CPU \d+, with 1024 MB of memory', line), out

def check_counter(binary):
    for variant in ('own', 'shared'):
        out = run(binary, '-c2', '-m1G', '--variant', variant, '--increments', '1000000')
        assert re.fullmatch(rf'2 shards, {variant} counters: [\d.]+ s, [\d.]+ ns per increment\n', out), out

def check_wrong_shard(binary):
    # With more than one shard, the program is wrong on purpose, and its
    # result unpredictable; on one shard, nothing is shared.
    out = run(binary, '-c1', '-m1G')
    assert out == '1 shards: use count 1, should be 1\n', out

def check_foreign(binary):
    out = run(binary, '-c2', '-m1G')
    assert out.splitlines() == [
        'shard 0 got a table that belongs to shard 1:',
        '  row 0, made on shard 1', '  row 1, made on shard 1', '  row 2, made on shard 1',
        'the table is destroyed on shard 1', 'back on shard 0'], out

def check_hop(binary):
    for args in (['--to', '0'], ['--to', '1'], ['--to', '1', '--in-flight', '100']):
        out = run(binary, '-c2', '-m1G', '--calls', '100000', *args)
        assert re.fullmatch(r'shard 0 to shard \d, \d+ in flight: \d+ ns per request, '
                            r'[\d.]+ million requests a second\n', out), out

def check_wordcount(binary):
    for variant in ('one', 'batched'):
        out = run(binary, '-c3', '-m1G', '--variant', variant, '--words', '100000')
        assert re.fullmatch(rf'3 shards, {variant}: 100000 words, 100000 different, '
                            r'in \d+ ns per word\n', out), out

def check_backlog(binary):
    # The connections beyond the backlog (Linux takes one more) wait for the
    # client's SYN to be sent again, a second later.
    out = run(binary, '-c1', '-m1G')
    m = re.fullmatch(r'backlog 100: (\d+) connections in under 0.1 s, (\d+) took a second '
                     r'or more \(the slowest [\d.]+ s\)\n'
                     r'backlog 1000: 300 connections in under 0.1 s, 0 took a second or more '
                     r'\(the slowest [\d.]+ s\)\n', out)
    assert m and int(m.group(1)) <= 110 and int(m.group(2)) >= 150, out

def check_nested(binary):
    # The first case deadlocks, until the nested call's 1-second timeout.
    out = run(binary, '-c2', '-m1G')
    assert re.fullmatch(r'one group: seastar::named_semaphore_timed_out \(Semaphore timed out: '
                        r'smp_service_group#\d+ \d->\d semaphore\)\n'
                        r'two groups: done\n', out), out

def check_startup(binary):
    out = run(binary, '-c3', '-m1G')
    assert out.splitlines() == [
        'hello from shard 0', 'hello from shard 1', 'hello from shard 2',
        'shard 1 has an instance: false',
        'shard 1 asks shard 0: hello from the only instance',
        'shard 1 asks shard 1: sharded instance does not exist: greeter'], out

def check_ptr_cost(binary):
    for variant in ('raw', 'lw', 'seastar', 'std', 'std-shared'):
        out = run(binary, '-c2', '-m1G', '--variant', variant, '--calls', '1000000')
        assert re.fullmatch(rf'2 shards, {variant}: [\d.]+ ns per call\n', out), out

def check_shared_nothing_ex1(binary):
    out = run(binary, '-c2', '-m1G')
    assert out == ('shard 0 got 3 rows\n'
                   'destroying them on shard 0 took 4 cross-shard frees\n'), out

def check_shared_nothing_ex2(binary):
    out = run(binary, '-c3', '-m1G')
    assert out.splitlines() == [
        'shard 0 has two pointers, owned by shards 1 and 1',
        'shard 2 reads 2 rows, and waits', 'shard 0 drops its pointer',
        'the table is destroyed on shard 1', 'shard 0 is done'], out

def check_shared_nothing_ex3(binary):
    # Only a debug build notices the copy variant's bug; this checks that
    # both variants run.
    for variant in ('read', 'copy'):
        out = run(binary, '-c2', '-m1G', '--variant', variant)
        assert out == 'shard 1 saw 1000 elements; use count 1, should be 1\n', out

def check_shared_nothing_ex4(binary):
    out = run(binary, '-c4', '-m4G')
    lines = out.splitlines()
    assert lines[:3] == ['shard 0 allocated 512 MB', "shard 0 couldn't allocate 900 MB",
                         "shard 0 couldn't allocate 2048 MB"], out
    assert sorted(l.split(':')[0] for l in lines[3:]) == [f'shard {i}' for i in range(4)], out

def check_case_for_seastar_ex2(binary):
    for variant in ('shared', 'padded', 'adjacent', 'plain'):
        out = run(binary, variant, '2')
        assert re.fullmatch(rf'{variant}, 2 threads: [\d.]+ s, [\d.]+ ns per increment\n', out), out

def check_tutorial_introduction_ex1(binary):
    assert run(binary, '-c1', '--seconds', '0') == 'Hello, world\n'

def check_tutorial_introduction_ex2(binary):
    out = run(binary, '-c1')
    ends = re.findall(r'^(\w+) ends after (\d+)ms on shard 0$', out, re.M)
    assert [name for name, _ in ends] == ['tack', 'tick', 'tock'], out
    for (name, ms), expected in zip(ends, (800, 900, 1000)):
        assert expected <= int(ms) < expected + 150, out

def check_tutorial_introduction_ex3(binary):
    with server(binary, '-c1'):
        s = connect(1234)
        s.sendall(b'hel')
        time.sleep(0.3)
        s.sendall(b'lo\nwor')
        time.sleep(0.3)
        s.sendall(b'ld\n')
        got = b''
        while got.count(b'\n') < 2:
            got += s.recv(100)
        s.close()
    assert got == b'hello\nworld\n', got

def check_tutorial_introduction_ex4(binary):
    with server(binary, '-c4') as srv:
        for port in (50000, 50001, 50002, 50003, 50005, 50010):
            s = socket.socket()
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            s.bind(('127.0.0.1', port))
            connect_deadline = time.time() + 10
            while True:
                try:
                    s.connect(('127.0.0.1', 1234))
                    break
                except ConnectionRefusedError:
                    assert time.time() < connect_deadline
                    time.sleep(0.1)
            s.sendall(b'hi\n')
            assert s.recv(100) == b'hi\n'
            s.close()
    assert {port: shard for shard, port in re.findall(r'shard (\d) serves 127\.0\.0\.1:(\d+)', srv.output)} == \
        {'50000': '0', '50001': '1', '50002': '2', '50003': '3', '50005': '1', '50010': '2'}, srv.output

def check_getting_started_ex2(binary):
    for args, env, expected in (([], None, 'hello'), (['--greeting', 'hi'], None, 'hi'),
                                ([], '--greeting "good morning"', 'good morning'),
                                (['--greeting', 'hey'], '--greeting hi', 'hey')):
        environment = dict(os.environ, SEASTAR_OPTIONS=env) if env else None
        p = subprocess.run([binary, '-c2', *args], capture_output=True, text=True, env=environment)
        assert p.returncode == 0, p.stderr
        assert sorted(re.findall(r'shard (\d) says: (.*)$', p.stderr, re.M)) == \
            [('0', expected), ('1', expected)], p.stderr

def check_coroutines_ex5(binary):
    out = run(binary, '-c1', '-m1G')
    rows = re.findall(r'^(.+?)\s+(\d+) ns each \((\d+) errors\)$', out, re.M)
    assert [(what, errors) for what, _, errors in rows] == [
        ('coroutine, success', '0'), ('ordinary function, success', '0'),
        ('coroutine, failure', '1000000'), ('ordinary function, failure', '1000000')], out

def check_shoal_naive_keys(binary):
    # The chapter's wrong KEYS: every other test must pass, and
    # test_keys_while_changing fails in some runs, which isn't checked.
    print(f'  other tests: '
          f'{run_tests("v2", "--shoal", binary, "-k", "not keys_while_changing")}')
    failures = 0
    for _ in range(3):
        p = subprocess.run([str(book / 'shoal' / 'test' / 'run'), 'v2', '-q', '--shoal', binary,
                            '-k', 'keys_while_changing'], capture_output=True, text=True,
                           timeout=600)
        failures += p.returncode != 0
    print(f'  test_keys_while_changing failed in {failures} of 3 runs')

def check_futures_and_promises_ex1(binary):
    # get() on a future that isn't available, outside a Seastar thread,
    # aborts the program: the exercise's point.
    p = subprocess.run([binary, '-c1', '-m1G'], capture_output=True, text=True, timeout=60)
    assert p.returncode != 0 and 'Assertion `thread` failed' in p.stderr, p.stderr

def check_futures_and_promises_ex2(binary):
    assert run(binary, '-c1', '-m1G', '--variant', 'broken').splitlines() == [
        '1 failed: the disk is busy', '2 failed: the disk is busy',
        '3 failed: the disk is busy', '1 loads']
    assert run(binary, '-c1', '-m1G', '--variant', 'fixed').splitlines() == [
        '1 failed: the disk is busy', '2 got the value of x',
        '3 got the value of x', '2 loads']

def check_futures_and_promises_ex3(binary):
    assert run(binary, '-c1', '-m1G').splitlines() == [
        'first signal', 'waiter 1 woke up', 'waiter 2 woke up',
        'second signal', 'waiter 3 woke up']

def check_futures_and_promises_ex4(binary):
    p = subprocess.run([binary, '-c1', '-m1G'], capture_output=True, text=True, timeout=60)
    assert p.returncode == 0 and p.stdout == 'dropped the future\ndone\n', p.stdout
    assert 'Exceptional future ignored' not in p.stderr, p.stderr

# object-lifetime-ex1, object-lifetime-ex2 and time-ex3 use freed memory
# on purpose, so what they do isn't defined; coordination-ex3 and
# errors-ex1 and clean-shutdown-ex5 hang, and coordination-ex4 aborts, on
# purpose; errors-ex2 runs for minutes. They are only built.
def check_clean_shutdown_ex2(binary):
    p = subprocess.run([binary, '-c1', '-m512M'], capture_output=True, text=True, timeout=60)
    assert p.returncode == 0, p.stderr
    assert 'sleep_abortable(): stopping took 0.0 seconds' in p.stdout, p.stdout
    assert 'Exceptional future ignored: seastar::sleep_aborted' in p.stderr, p.stderr

def check_memory_leaks_ex1(binary):
    assert run(binary, '-c1', '-m512M') == 'main is done\n'

def check_talking_between_shards_ex2(binary):
    out = run(binary, '-c3', '-m1G', '--words', '100000')
    assert re.fullmatch(r'3 shards, every shard makes words: 100000 words, 100000 different, '
                        r'in \d+ ns per word\n', out), out

def check_talking_between_shards_ex3(binary):
    out = run(binary, '-c3', '-m1G', '--words', '10000')
    lines = out.splitlines()
    assert lines[0].startswith('3 shards, batched: 10000 words'), out
    assert lines[1] == 'shard 0 stops', out
    assert sorted(lines[2:]) == ['shard 1 stops', 'shard 2 stops'], out

def check_talking_between_shards_ex4(binary):
    for units, calls, result in ((2, 1, 'done'), (2, 2, 'seastar::named_semaphore_timed_out')):
        out = run(binary, '-c2', '-m1G', '--units', str(units), '--calls', str(calls))
        assert out.startswith(f'{units} units, {calls} calls at once: {result}'), out

def check_clean_shutdown_ex3(binary):
    out = run(binary, '-c1', '-m512M')
    assert out == 'subscription engaged: false, function called: false\n', out

def check_errors_ex3(binary):
    # The same output as kinds.cc, with exception_ptr_cast or without.
    check_kinds(binary)

def check_errors_ex4(binary):
    p = subprocess.run([binary, '-c1', '-m512M'], capture_output=True, text=True, timeout=60)
    assert p.returncode != 0 and 'Aborting on shard 0' in p.stderr, p.stderr
    assert 'app - negative balance -50 after withdrawing 150, at: 0x' in p.stderr, p.stderr
    assert 'goes on' not in p.stdout, p.stdout

def check_errors_ex5(binary):
    lines = run(binary, '-c1', '-m512M').splitlines()
    assert lines[0].startswith('the withdrawal failed: negative balance -50 '
                               'after withdrawing 150 Backtrace: 0x'), lines
    assert lines[-1] == 'the deposit failed', lines

def check_object_lifetime_ex2_fixed(binary):
    assert run(binary, '-c1', '-m1G') == 'smp count: 1\n'

def check_object_lifetime_ex3(binary):
    for variant in ('raw', 'lw', 'seastar', 'std', 'std-shared'):
        out = run(binary, '-c2', '-m1G', '--variant', variant, '--calls', '1000000')
        assert re.fullmatch(rf'2 shards, {variant}: [\d.]+ ns per call\n', out), out

def check_object_lifetime_ex4(binary):
    # Only the variant that awaits in the same statement is correct.
    out = run(binary, '-c1', '-m1G', '--variant', 'statement')
    assert out == 'statement: the string has 100 characters\n', out

checks = {
    'object-lifetime-ex2-fixed': check_object_lifetime_ex2_fixed,
    'errors-ex3': check_errors_ex3,
    'clean-shutdown-ex2': check_clean_shutdown_ex2,
    'clean-shutdown-ex3': check_clean_shutdown_ex3,
    'talking-between-shards-ex2': check_talking_between_shards_ex2,
    'talking-between-shards-ex3': check_talking_between_shards_ex3,
    'talking-between-shards-ex4': check_talking_between_shards_ex4,
    'memory-leaks-ex1': check_memory_leaks_ex1,
    'errors-ex4': check_errors_ex4,
    'errors-ex5': check_errors_ex5,
    'object-lifetime-ex3': check_object_lifetime_ex3,
    'object-lifetime-ex4': check_object_lifetime_ex4,
    'futures-and-promises-ex1': check_futures_and_promises_ex1,
    'futures-and-promises-ex2': check_futures_and_promises_ex2,
    'futures-and-promises-ex3': check_futures_and_promises_ex3,
    'futures-and-promises-ex4': check_futures_and_promises_ex4,
    'shoal-naive-keys': check_shoal_naive_keys,
    'coroutines-ex5': check_coroutines_ex5,
    'case-for-seastar-ex2': check_case_for_seastar_ex2,
    'tutorial-introduction-ex1': check_tutorial_introduction_ex1,
    'tutorial-introduction-ex2': check_tutorial_introduction_ex2,
    'tutorial-introduction-ex3': check_tutorial_introduction_ex3,
    'tutorial-introduction-ex4': check_tutorial_introduction_ex4,
    'getting-started-ex2': check_getting_started_ex2,
    'shared-nothing-ex1': check_shared_nothing_ex1, 'shared-nothing-ex2': check_shared_nothing_ex2,
    'shared-nothing-ex3': check_shared_nothing_ex3, 'shared-nothing-ex4': check_shared_nothing_ex4,
    'owns': check_owns, 'counter': check_counter, 'wrong-shard': check_wrong_shard,
    'foreign': check_foreign, 'ptr-cost': check_ptr_cost,
    'hop': check_hop, 'wordcount': check_wordcount, 'nested': check_nested,
    'backlog': check_backlog, 'udp': check_udp,
    'random-reads': check_random_reads,
    'reads-and-writes': check_reads_and_writes,
    'cancel': check_cancel,
    'startup': check_startup,
    'states': check_states, 'promise': check_promise, 'shared': check_shared,
    'ignored': check_ignored, 'cost': check_cost, 'throwing': check_throwing,
    'kinds': check_kinds, 'internal': check_internal, 'expected': check_expected,
    'overload': check_overload,
    'periodic': check_periodic, 'abandon': check_abandon,
    'cycle': check_cycle, 'stuck': check_stuck,
    'idioms': check_idioms,
    'lessons-from-large-code-bases-ex1': check_lessons_from_large_code_bases_ex1,
    'lessons-from-large-code-bases-ex2': check_lessons_from_large_code_bases_ex2,
    'deploying-and-tuning-ex1': check_deploying_and_tuning_ex1,
    'deploying-and-tuning-ex2': check_deploying_and_tuning_ex2,
    'memory-leaks-ex2': check_memory_leaks_ex2, 'memory-leaks-ex3': check_memory_leaks_ex3,
    'shoal-v3': check_shoal_v3,
    'shoal-v4': check_shoal_v4,
    'shoal-v5': check_shoal_v5,
    'shoal-v6': check_shoal_v6,
    'shoal-v7': check_shoal_v7,
    'shoal-v8': check_shoal_v8, 'shoal-v9': check_shoal_v9,
    'shoal-v10': check_shoal_v10,
    'withdraw': check_withdraw, 'units': check_units, 'deadlock': check_deadlock,
    'semaphore-cost': check_semaphore_cost, 'condition': check_condition,
    'gate': check_gate, 'queue': check_queue, 'rwlock': check_rwlock,
    'clocks': check_clocks, 'timers': check_timers, 'timeouts': check_timeouts,
    'manual': check_manual,
    'pubsub-bench': check_pubsub_bench,
    'errors': check_errors, 'count-bytes': check_count_bytes,
    'fiasco': check_fiasco, 'sharing': check_sharing, 'order': check_order,
    'closing': check_closing, 'cpu-shares': check_cpu_shares,
    'fibers': check_fibers,
    'which-group': check_which_group, 'group-data': check_group_data,
    'scheduling-groups-ex1': check_scheduling_groups_ex1,
    'scheduling-groups-ex2': check_scheduling_groups_ex2,
    'shoal-v11': check_shoal_v11,
    'shoal-v12': check_shoal_v12,
    'allocations': check_allocations, 'cross-free': check_cross_free,
    'reclaimer': check_reclaimer,
    'memory-ex2': check_memory_ex2, 'memory-ex3': check_memory_ex3,
    'basics': check_basics, 'deferred': check_deferred, 'long-work': check_long_work,
    'alien': check_alien, 'worker': check_worker, 'process': check_process,
    'threads-ex1': check_threads_ex1, 'threads-ex2': check_threads_ex2,
    'threads-ex3': check_threads_ex3, 'threads-ex4': check_threads_ex4,
    'threads-ex5': check_threads_ex5,
    'sources': check_sources, 'gentle': check_gentle, 'threshold': check_threshold,
    'stalls-ex2': check_stalls_ex2, 'stalls-ex3': check_stalls_ex3,
    'stalls-ex4': check_stalls_ex4, 'stalls-ex5': check_stalls_ex5,
    'count-bytes-then': check_count_bytes, 'errors-then': check_errors_then,
    'when-all': check_when_all, 'cost-then': check_cost_then,
    'continuations-ex1': check_when_all, 'continuations-ex2': check_continuations_ex2,
    'continuations-ex3': check_continuations_ex3, 'continuations-ex4': check_cost_then,
    'continuations-ex5': check_count_bytes,
    'run-order': check_run_order, 'preempt': check_preempt,
    'awaitable': check_awaitable, 'generators': check_generators,
    'how-coroutines-work-ex1': check_how_coroutines_work_ex1,
    'how-coroutines-work-ex2': check_how_coroutines_work_ex2,
    'how-coroutines-work-ex3': check_how_coroutines_work_ex3,
    'how-coroutines-work-ex4': check_how_coroutines_work_ex4,
    'how-coroutines-work-ex5': check_how_coroutines_work_ex5,
    'shoal-v13': check_shoal_v13, 'retry': check_retry, 'http-ex4': check_http_ex4,
    'shoal-v14': check_shoal_v14, 'verbs': check_verbs,
    'shoal-v15': check_shoal_v15, 'shoal-v16': check_shoal_v16,
    'table-fuzz': check_table_fuzz, 'testing-ex2': check_testing_ex2,
    'preemption': check_preemption, 'count-lines': check_count_lines,
    'shoal-v0': check_shoal_v0,
    'shoal-v1': check_shoal_v1,
    'shoal-v2': check_shoal_v2,
    'line-server': check_line_server, 'count-keys': check_count_keys,
    'hello': check_hello, 'shards': check_shards, 'sleep': check_sleep,
    'echo': check_echo, 'echo-all-cores': check_echo_all_cores, 'pong': check_pong,
}

def compile_example(src, cxx, flags, out_dir):
    """Compiles one example file, as the book tells readers to: with
    pkg-config's flags, which include Seastar's C++ standard. Returns the
    binary and its compile command, or None."""
    binary = out_dir / src.stem
    cmd = [cxx, '-O2', '-Wall', '-Werror', str(src), '-o', str(binary), *flags]
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        print(p.stderr)
        return None
    return binary, ' '.join(cmd)

def update_patched(patch, src_dir):
    """Makes src_dir the Shoal that patch makes, rewriting only the files
    whose contents changed, so that a build of src_dir stays incremental."""
    with tempfile.TemporaryDirectory() as tmp:
        new = shoal_patch.apply(patch, Path(tmp) / 'shoal')
        wanted = {f.relative_to(new) for f in new.rglob('*') if f.is_file()}
        src_dir.mkdir(parents=True, exist_ok=True)
        for f in src_dir.rglob('*'):
            if f.is_file() and f.relative_to(src_dir) not in wanted:
                f.unlink()
        for rel in wanted:
            dest = src_dir / rel
            if not dest.exists() or dest.read_bytes() != (new / rel).read_bytes():
                dest.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(new / rel, dest)

def build_shoal(src_dir, cxx, out_dir):
    """Builds one version of Shoal with CMake, as the book tells readers to,
    without choosing a C++ standard. Returns the binary and the command that
    compiled main.cc, or None."""
    build_dir = out_dir / src_dir.name
    # A build directory made for another source directory (such as a
    # solution's, before solutions were patches) can't be reused.
    cache = build_dir / 'CMakeCache.txt'
    if cache.exists() and f'CMAKE_HOME_DIRECTORY:INTERNAL={src_dir}\n' not in cache.read_text():
        shutil.rmtree(build_dir)
    configure = subprocess.run(['cmake', '-S', str(src_dir), '-B', str(build_dir), '-G', 'Ninja',
                                f'-DCMAKE_CXX_COMPILER={cxx}',
                                '-DCMAKE_CXX_FLAGS=-Wall -Werror'],
                               capture_output=True, text=True)
    if configure.returncode != 0:
        print(configure.stdout[-3000:], configure.stderr[-3000:])
        return None
    build = subprocess.run(['cmake', '--build', str(build_dir)], capture_output=True, text=True)
    if build.returncode != 0:
        print(build.stdout[-5000:])
        return None
    commands = subprocess.run(['ninja', '-C', str(build_dir), '-t', 'commands', 'shoal'],
                              capture_output=True, text=True).stdout
    command = next((l for l in commands.splitlines() if 'main.cc' in l), '')
    return build_dir / 'shoal', command

def used_standard(command):
    """The C++ standard that a compile command asked for, such as '23'."""
    m = re.search(r'-std=(?:gnu|c)\+\+(\d+)', command)
    return m.group(1) if m else None

def check_toolchain(cxx, std, out, names):
    """Builds and checks every program with one compiler, against the Seastar
    found through the environment, which was built for the C++ standard std
    (or an unknown one, if std is empty); returns the list of failures."""
    flags = subprocess.run(['pkg-config', '--cflags', '--libs', '--static', 'seastar'],
                           capture_output=True, text=True, check=True).stdout.split()
    # Each program: its name (for checks and NAMEs), how to show it, and how
    # to build it.
    programs = []
    for src in sorted(book.glob('examples/*/*.cc')):
        out_dir = out / src.parent.name
        out_dir.mkdir(parents=True, exist_ok=True)
        programs.append((src.stem, str(src.relative_to(book)),
                         lambda src=src, out_dir=out_dir: compile_example(src, cxx, flags, out_dir)))
    for src_dir in sorted(book.glob('shoal/v*/'), key=shoal_version):
        (out / 'shoal').mkdir(parents=True, exist_ok=True)
        programs.append((f'shoal-{src_dir.name}', str(src_dir.relative_to(book)),
                         lambda src_dir=src_dir: build_shoal(src_dir, cxx, out / 'shoal')))
    # The solutions that change Shoal, each a patch to a version of it: the
    # patch is applied to a copy of that version, which is built, and checked
    # with that version's tests and the solution's own, against Valkey and
    # against it.
    for patch in sorted(book.glob('examples/solutions/shoal-*.patch')):
        src_dir = out / 'solutions' / 'src' / patch.stem
        def build(patch=patch, src_dir=src_dir):
            try:
                update_patched(patch, src_dir)
            except RuntimeError as e:
                print(e)
                return None
            return build_shoal(src_dir, cxx, out / 'solutions')
        programs.append((patch.stem, str(patch.relative_to(book)), build))
        checks.setdefault(patch.stem, lambda binary, patch=patch, src_dir=src_dir:
                          check_solution(patch, src_dir, binary))
    if names:
        unknown = set(names) - {p[0] for p in programs}
        if unknown:
            print(f'no such programs: {" ".join(sorted(unknown))}')
            return [f'unknown {name}' for name in sorted(unknown)]
        programs = [p for p in programs if p[0] in names]
    failed = []
    label = f'C++{std}' if std else 'its C++ standard'
    for name, shown, build in programs:
        built = build()
        if not built:
            print(f'{shown} with {label} ({cxx}): FAILED to build')
            failed.append(f'{name} {label} {cxx}')
            continue
        binary, command = built
        # The program must have been compiled with Seastar's own standard,
        # which neither the book's commands nor Shoal's CMakeLists.txt choose.
        if std and used_standard(command) != std:
            print(f'{shown} ({cxx}): compiled with C++{used_standard(command)}, not C++{std}')
            failed.append(f'{name} {label} {cxx}: wrong standard')
            continue
        print(f'{shown} with {label} ({cxx}): built')
        if name not in checks:
            print(f'{shown}: no run check')
            continue
        try:
            checks[name](str(binary))
            print(f'{shown} ({cxx}): runs correctly')
        except Exception as e:
            print(f'{shown} ({cxx}): run FAILED: {e}')
            failed.append(f'{name} run {cxx}')
    return failed

def shoal_version(src_dir):
    """The version number of a Shoal directory, such as (2,) for v2, or (1, 1) for v1.1."""
    return tuple(int(n) for n in src_dir.name[1:].split('.'))

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--seastar', action='append', default=[],
                        metavar='CXX[:STD]=PREFIX[:PREFIX...]',
                        help='a compiler and the Seastar built with it (repeatable)')
    parser.add_argument('--out', type=Path, default=book / 'build' / 'examples')
    parser.add_argument('names', nargs='*')
    args = parser.parse_args()
    failed = []
    if not args.seastar:
        failed += check_toolchain("g++", '', args.out / "g++", args.names)
    for spec in args.seastar:
        compiler, prefixes = spec.split('=', 1)
        cxx, _, std = compiler.partition(':')
        prefixes = [str(Path(p).expanduser()) for p in prefixes.split(':')]
        os.environ['CMAKE_PREFIX_PATH'] = ':'.join(prefixes)
        os.environ['PKG_CONFIG_PATH'] = ':'.join(
            f'{p}/{lib}/pkgconfig' for p in prefixes for lib in ('lib64', 'lib'))
        failed += check_toolchain(cxx, std, args.out / (f'{cxx}-c++{std}' if std else cxx),
                                  args.names)
    if failed:
        sys.exit('failed: ' + ', '.join(failed))
    print('all checks passed')

main()
