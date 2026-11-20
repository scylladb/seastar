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
# Copyright (C) 2026 ScyllaDB

"""
Tests of scripts/seastar-addr2line and scripts/stall-analyser.py with the real
tools, on a small executable with debug info built by the test.

The tests check what the scripts promise and the tools agree on: the function
names, the order and nesting of the frames and the marking of inlined
functions. They leave out what differs between the tools and their versions,
like the file paths, the line numbers, the discriminators and the function
name printed for the inlined frame.
"""

import re
import unittest

from testcase import ADDR2LINES, RealToolsTestCase


class TestRealTools(RealToolsTestCase):
    """Runs the scripts with every installed addr2line on a real executable."""

    def stall(self, *addresses: str) -> str:
        """Returns a stall report of 100 ms with the given addresses, innermost frame first."""
        return f'Reactor stalled for 100 ms on shard 0. Backtrace: {" ".join(addresses)}\n'

    def test_resolve(self):
        """seastar-addr2line resolves the functions of a backtrace, innermost first."""
        a = self.addresses
        for addr2line in ADDR2LINES:
            with self.subTest(addr2line):
                res = self.run_script_named('seastar-addr2line', '--executable', self.debug_executable,
                                            '--addr2line', addr2line, a['outer'], a['main'])
                self.assertEqual(res.returncode, 0, res.stderr)
                self.assertRegex(res.stdout, r'(?s)\[Backtrace #0\]\nouter at .*\nmain at ')

    def test_resolve_inlined(self):
        """seastar-addr2line shows the inlined function and the function it is inlined into."""
        for addr2line in ADDR2LINES:
            with self.subTest(addr2line):
                res = self.run_script_named('seastar-addr2line', '--executable', self.debug_executable,
                                            '--addr2line', addr2line, self.addresses['inlined'])
                self.assertEqual(res.returncode, 0, res.stderr)
                self.assertRegex(res.stdout, r'(?s)\ninner at .*\n \(inlined by\) middle at ')

    def test_missing_library(self):
        """An address in a missing library is printed as it is, and the others get resolved."""
        for addr2line in ADDR2LINES:
            with self.subTest(addr2line):
                res = self.run_script_named('seastar-addr2line', '--executable', self.debug_executable,
                                            '--addr2line', addr2line,
                                            input=f'Backtrace: /nonexistent/lib.so+0x32 {self.addresses["main"]}\n')
                self.assertEqual(res.returncode, 0, res.stderr)
                self.assertIn('/nonexistent/lib.so 0x32', res.stdout)
                self.assertIn('main at ', res.stdout)

    def test_missing_debug_info(self):
        """seastar-addr2line warns about an executable without debug info only."""
        for executable, warned in [(self.debug_executable, False), (self.nodebug_executable, True)]:
            with self.subTest(executable):
                res = self.run_script_named('seastar-addr2line', '--executable', executable,
                                            self.addresses['main'])
                self.assertEqual(res.returncode, 0, res.stderr)
                self.assertEqual('ELF' in res.stdout, warned, res.stdout)

    def test_stall_graph(self):
        """The graph names the functions and the inlined frame in every stall report frame."""
        a = self.addresses
        for addr2line in ADDR2LINES:
            with self.subTest(addr2line):
                res = self.run_script_named('stall-analyser.py', '--executable', self.debug_executable,
                                            '--addr2line', addr2line,
                                            input=self.stall(a['inlined'], a['outer'], a['main']))
                self.assertEqual(res.returncode, 0, res.stderr)
                self.assertRegex(res.stdout, r'(?s)inner at .*\(inlined by\) middle at .*outer at .*main at ')

    def test_stall_trace(self):
        """The trace format shows the call chain outermost first, with the inlined frame marked."""
        a = self.addresses
        for addr2line in ADDR2LINES:
            with self.subTest(addr2line):
                res = self.run_script_named('stall-analyser.py', '--executable', self.debug_executable,
                                            '--addr2line', addr2line, '--format', 'trace',
                                            input=self.stall(a['inlined'], a['outer'], a['main']))
                self.assertEqual(res.returncode, 0, res.stderr)
                self.assertRegex(res.stdout, r'^main;outer;middle;\w+_\[i\] 100\n$')

    def test_stall_trace_without_inlined_frames(self):
        """No frame of the trace format is marked as inlined when no function is inlined."""
        a = self.addresses
        res = self.run_script_named('stall-analyser.py', '--executable', self.debug_executable,
                                    '--format', 'trace', input=self.stall(a['outer'], a['main']))
        self.assertEqual(res.returncode, 0, res.stderr)
        self.assertEqual(res.stdout, 'main;outer 100\n')


if __name__ == '__main__':
    unittest.main()
