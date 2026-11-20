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
# Copyright (C) 2026 ScyllaDB
#

"""
Helpers for the end-to-end tests of the scripts in scripts/.

Each test runs a script as a subprocess, in a temporary working directory, and
checks its output.

Two kinds of tests exist:
  - ScriptTestCase tests check the exact output of the scripts. They run the
    fake tools in fake-bin/, which the scripts find first on PATH, and only
    run when the environment variable SCRIPTS_TEST_FAKES is set.
  - RealToolsTestCase tests check the parts of the output the scripts promise
    on a small executable built with debug info, resolved by the file(1),
    llvm-addr2line and addr2line found on PATH. They run unless
    SCRIPTS_TEST_FAKES is set, and skip the tools which are not installed.

Setting the SCRIPTS_COVERAGE environment variable to a path runs the scripts
under coverage.py, which writes its data files next to that path. Combine and
report them with:
  python3 -m coverage combine --data-file=$SCRIPTS_COVERAGE
  python3 -m coverage report --data-file=$SCRIPTS_COVERAGE
"""

import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPTS_DIR = Path(__file__).resolve().parents[2] / 'scripts'
FAKE_BIN_DIR = Path(__file__).resolve().parent / 'fake-bin'
# set to run the tests with the fake tools instead of the real ones
USE_FAKES = bool(os.environ.get('SCRIPTS_TEST_FAKES'))
# made-up executable created in the working directory of every test
EXECUTABLE = 'gibberish'


def script_command(script: str) -> list[str]:
    """Returns the command line prefix which runs the given script, under coverage.py if requested."""
    command = [sys.executable]
    if os.environ.get('SCRIPTS_COVERAGE'):
        command += ['-m', 'coverage', 'run', '--parallel-mode', '--branch']
    return command + [str(SCRIPTS_DIR / script)]


def script_env() -> dict[str, str]:
    """
    Returns the environment the scripts run in.

    With the fake tools, PATH starts with fake-bin/, so the scripts run the
    fake llvm-addr2line, c++filt and file(1).
    """
    env = dict(os.environ, PYTHONUTF8='1')
    if USE_FAKES:
        env['PATH'] = os.pathsep.join([str(FAKE_BIN_DIR), env.get('PATH', os.defpath)])
    coverage_file = os.environ.get('SCRIPTS_COVERAGE')
    if coverage_file:
        env['COVERAGE_FILE'] = coverage_file
    return env


class ScriptTestCase(unittest.TestCase):
    """
    Base of the tests of one script, named by the SCRIPT class attribute.

    Every test runs in its own temporary working directory, which contains
    the made-up executable EXECUTABLE. The tests need the fake tools.
    """

    SCRIPT = ''

    def setUp(self):
        """Creates the working directory of the test with EXECUTABLE in it."""
        if not USE_FAKES:
            self.skipTest('runs with the fake tools, set SCRIPTS_TEST_FAKES to run it')
        workdir = tempfile.TemporaryDirectory()
        self.addCleanup(workdir.cleanup)
        self.workdir = Path(workdir.name)
        self.write_file(EXECUTABLE, 'Not a binary, addresses in it are resolved by the fake llvm-addr2line.\n')

    def write_file(self, name: str, content: str) -> str:
        """Writes the file with the given content to the working directory and returns its name."""
        (self.workdir / name).write_text(content)
        return name

    def run_script(self, *args: str, **kwargs) -> subprocess.CompletedProcess:
        """
        Runs the script with the given arguments in the working directory.

        The keyword arguments are passed to subprocess.run(); stdout and stderr
        are captured as text unless given.
        """
        return self.run_script_named(self.SCRIPT, *args, **kwargs)

    def run_script_named(self, script: str, *args: str, **kwargs) -> subprocess.CompletedProcess:
        """Runs the given script like run_script() does."""
        kwargs.setdefault('stdout', subprocess.PIPE)
        kwargs.setdefault('stderr', subprocess.PIPE)
        kwargs.setdefault('check', False)
        return subprocess.run(script_command(script) + list(args),
                              cwd=self.workdir, env=script_env(), text=True,
                              timeout=120, **kwargs)


# C fixture with a function inlined into another one, called from a third one
FIXTURE_SOURCE = """\
static inline __attribute__((always_inline)) int inner(volatile int *p)
{
    return *p + 1;
}

__attribute__((noinline)) int middle(volatile int *p)
{
    return inner(p) * 2;
}

__attribute__((noinline)) int outer(volatile int *p)
{
    return middle(p) + 3;
}

int main(void)
{
    volatile int v = 1;
    return outer(&v);
}
"""
ADDR2LINES = ['llvm-addr2line', 'addr2line']


class RealToolsTestCase(ScriptTestCase):
    """
    Base of the tests of one script on a real executable, resolved by the real tools.

    The class attributes hold the fixture executables, built once per class:
      debug_executable - has debug info; contains the functions main, outer,
                         middle and inner, which is inlined into middle
      nodebug_executable - the same without debug info
    Every test runs in its own temporary working directory, with the
    fixtures symlinked in.
    """

    debug_executable = ''
    nodebug_executable = ''
    addresses: dict[str, str] = {}

    @classmethod
    def setUpClass(cls):
        """Builds the fixture executables, skipping the tests when the tools are not available."""
        if USE_FAKES:
            raise unittest.SkipTest('runs with the real tools, unset SCRIPTS_TEST_FAKES to run it')
        compiler = os.environ.get('CC') or shutil.which('cc') or shutil.which('gcc') or shutil.which('clang')
        for tool in ['file', 'nm', 'c++filt', 'llvm-addr2line', 'addr2line']:
            if not shutil.which(tool):
                raise unittest.SkipTest(f'{tool} is not installed')
        if not compiler:
            raise unittest.SkipTest('no C compiler found')
        build_dir = tempfile.TemporaryDirectory()
        cls.addClassCleanup(build_dir.cleanup)
        source = Path(build_dir.name) / 'fixture.c'
        source.write_text(FIXTURE_SOURCE)
        for name, flags in [('fixture', ['-g']), ('fixture-nodebug', [])]:
            subprocess.run([compiler, '-O1', '-fno-pie', '-no-pie', *flags, '-o', name, str(source)],
                           cwd=build_dir.name, check=True)
        cls.debug_executable = str(Path(build_dir.name) / 'fixture')
        cls.nodebug_executable = str(Path(build_dir.name) / 'fixture-nodebug')
        cls.addresses = cls.symbol_addresses(cls.debug_executable)
        cls.addresses['inlined'] = cls.inlined_address(cls.debug_executable)

    @staticmethod
    def symbol_addresses(executable: str) -> dict[str, str]:
        """Returns the addresses of the fixture functions main, outer and middle, as hexadecimal numbers with 0x."""
        out = subprocess.run(['nm', executable], stdout=subprocess.PIPE, text=True, check=True).stdout
        found = {}
        for line in out.splitlines():
            address, _, name = line.partition(' T ')
            if name in ('main', 'outer', 'middle'):
                found[name] = hex(int(address, 16))
        return found

    @classmethod
    def inlined_address(cls, executable: str) -> str:
        """Returns the first address in middle() which addr2line resolves to the inlined inner(), too."""
        start = int(cls.addresses['middle'], 16)
        end = int(cls.addresses['outer'], 16)
        probes = [hex(address) for address in range(start, end)]
        out = subprocess.run(['addr2line', '-f', '-i', '-a', '-e', executable, *probes],
                             stdout=subprocess.PIPE, text=True, check=True).stdout
        for block in out.split('0x')[1:]:
            lines = block.splitlines()
            if 'inner' in lines:
                return '0x' + lines[0].lstrip('0')
        raise unittest.SkipTest('the compiler did not inline inner() into middle()')

    def setUp(self):
        """Creates the working directory of the test with the fixtures in it."""
        workdir = tempfile.TemporaryDirectory()
        self.addCleanup(workdir.cleanup)
        self.workdir = Path(workdir.name)
        for source in (self.debug_executable, self.nodebug_executable):
            (self.workdir / Path(source).name).symlink_to(source)
