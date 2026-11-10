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
checks its output. The scripts find the fake tools in fake-bin/ on PATH.

Setting the SCRIPTS_COVERAGE environment variable to a path runs the scripts
under coverage.py, which writes its data files next to that path. Combine and
report them with:
  python3 -m coverage combine --data-file=$SCRIPTS_COVERAGE
  python3 -m coverage report --data-file=$SCRIPTS_COVERAGE
"""

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPTS_DIR = Path(__file__).resolve().parents[2] / 'scripts'
FAKE_BIN_DIR = Path(__file__).resolve().parent / 'fake-bin'
# made-up executable created in the working directory of every test
EXECUTABLE = 'gibberish'


def script_command(script: str) -> list[str]:
    """Returns the command line prefix which runs the given script, under coverage.py if requested."""
    command = [sys.executable]
    if os.environ.get('SCRIPTS_COVERAGE'):
        command += ['-m', 'coverage', 'run', '--parallel-mode', '--branch']
    return command + [str(SCRIPTS_DIR / script)]


def script_env(env_extra: dict[str, str]) -> dict[str, str]:
    """
    Returns the environment the scripts run in, extended by env_extra.

    PATH starts with fake-bin/, so the scripts run the fake llvm-addr2line,
    c++filt and file(1).
    """
    env = dict(os.environ, PYTHONUTF8='1', **env_extra)
    env['PATH'] = os.pathsep.join([str(FAKE_BIN_DIR), env.get('PATH', os.defpath)])
    coverage_file = os.environ.get('SCRIPTS_COVERAGE')
    if coverage_file:
        env['COVERAGE_FILE'] = coverage_file
    return env


class ScriptTestCase(unittest.TestCase):
    """
    Base of the tests of one script, named by the SCRIPT class attribute.

    Every test runs in its own temporary working directory, which contains
    the made-up executable EXECUTABLE.
    """

    SCRIPT = ''

    def setUp(self):
        """Creates the working directory of the test with EXECUTABLE in it."""
        workdir = tempfile.TemporaryDirectory()
        self.addCleanup(workdir.cleanup)
        self.workdir = Path(workdir.name)
        self.write_file(EXECUTABLE, 'Not a binary, addresses in it are resolved by the fake llvm-addr2line.\n')

    def write_file(self, name: str, content: str) -> str:
        """Writes the file with the given content to the working directory and returns its name."""
        (self.workdir / name).write_text(content)
        return name

    def run_script(self, *args: str, env_extra: dict[str, str] | None = None,
                   **kwargs) -> subprocess.CompletedProcess:
        """
        Runs the script with the given arguments in the working directory.

        env_extra extends the environment of the script. The keyword arguments
        are passed to subprocess.run(); stdout and stderr are captured as text
        unless given.
        """
        kwargs.setdefault('stdout', subprocess.PIPE)
        kwargs.setdefault('stderr', subprocess.PIPE)
        return subprocess.run(script_command(self.SCRIPT) + list(args),
                              cwd=self.workdir, env=script_env(env_extra or {}), text=True,
                              timeout=120, **kwargs)
