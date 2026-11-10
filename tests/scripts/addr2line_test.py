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
#

"""
End-to-end tests of scripts/seastar-addr2line.

Each test resolves a few lines of input and checks the printed output.
Addresses get resolved by the fake fake-bin/llvm-addr2line in the made-up
executable EXECUTABLE:
  0x...0 -> ?? at ??:0
  0x...1 -> inlined_<addr> at gib/ber.hh:<addr % 97>
            (inlined by) caller_<addr>(int) at gib/ber.cc:<addr % 89>
  others -> func_<addr>(char const*) at gib/ber.cc:<addr % 101>
"""

import os
import pty
import unittest
from textwrap import dedent

from testcase import EXECUTABLE, ScriptTestCase

# kernel symbols resolving the addresses of KERNEL_CALLSTACK
KALLSYMS = '''\
ffffffff81000000 T _stext
ffffffff81000100 t gibberish_kernel_function
ffffffff81000200 T gibberish_last_symbol
'''
KERNEL_CALLSTACK = 'kernel callstack: 0xffffffff81000110 0xffffffff80000000'


class TestAddr2Line(ScriptTestCase):
    """Runs seastar-addr2line on backtraces and checks its output."""

    SCRIPT = 'seastar-addr2line'

    def resolve(self, *args: str, stdin: str = '') -> str:
        """Resolves stdin in EXECUTABLE with the given arguments, checks it succeeded and returns stdout."""
        res = self.run_script('--executable', EXECUTABLE, *args, input=stdin)
        self.assertEqual(res.returncode, 0, res.stderr)
        self.assertEqual(res.stderr, '')
        return res.stdout

    def test_addresses_as_arguments(self):
        """Addresses given as arguments form one backtrace; inlined frames take several lines."""
        output = self.resolve('0x12', '0x21', '0x30')
        self.assertEqual(output, dedent('''\
            [Backtrace #0]
            func_12(char const*) at gib/ber.cc:18
            inlined_21 at gib/ber.hh:33
             (inlined by) caller_21(int) at gib/ber.cc:33
            ?? at ??:0

            '''))

    def test_file(self):
        """With --file, the input is read from the file."""
        backtrace = self.write_file('backtrace.log', 'Backtrace: 0x12\n')
        output = self.resolve('--file', backtrace)
        self.assertEqual(output, dedent('''\
            Backtrace:
            [Backtrace #0]
            func_12(char const*) at gib/ber.cc:18

            '''))

    def test_file_and_addresses(self):
        """Giving both --file and addresses prints the usage, then resolves the file."""
        backtrace = self.write_file('backtrace.log', 'Backtrace: 0x12\n')
        output = self.resolve('--file', backtrace, '0x22')
        self.assertIn('Cannot use both -f and ADDRESS\nusage: ', output)
        self.assertTrue(output.endswith('[Backtrace #0]\nfunc_12(char const*) at gib/ber.cc:18\n\n'))

    def test_terminal(self):
        """Input typed on a terminal ends with an empty line."""
        controller, terminal = pty.openpty()
        try:
            os.write(controller, b'0x12\n\n0x22\n')
            res = self.run_script('--executable', EXECUTABLE, stdin=terminal)
        finally:
            os.close(terminal)
            os.close(controller)
        self.assertEqual(res.returncode, 0, res.stderr)
        self.assertEqual(res.stdout, dedent('''\
            [Backtrace #0]
            func_12(char const*) at gib/ber.cc:18

            '''))

    def test_one_line_backtrace(self):
        """A backtrace on one line is printed after the text preceding its addresses."""
        output = self.resolve(stdin='Reactor stalled on shard 1. Backtrace: 0x12 0x22 (BuildId: 0123abcd)\n')
        self.assertEqual(output, dedent('''\
            Reactor stalled on shard 1. Backtrace:
            [Backtrace #0]
            func_12(char const*) at gib/ber.cc:18
            func_22(char const*) at gib/ber.cc:34

            '''))

    def test_multi_line_backtrace(self):
        """A backtrace with one address per line is printed after the one line preceding it."""
        output = self.resolve(stdin='Segmentation fault on shard 0.\nBacktrace:\n  0x12\n  0x22\nnext line\n')
        self.assertEqual(output, dedent('''\
            Backtrace:
            [Backtrace #0]
            func_12(char const*) at gib/ber.cc:18
            func_22(char const*) at gib/ber.cc:34

            '''))

    def test_before(self):
        """--before sets the number of lines printed before a backtrace."""
        output = self.resolve('--before', '2', stdin='first line\nSegmentation fault on shard 0.\nBacktrace:\n  0x12\n')
        self.assertEqual(output, dedent('''\
            Segmentation fault on shard 0.
            Backtrace:
            [Backtrace #0]
            func_12(char const*) at gib/ber.cc:18

            '''))

    def test_before_zero(self):
        """With --before 0, only the backtraces are printed."""
        output = self.resolve('--before', '0', stdin='Segmentation fault on shard 0.\nBacktrace:\n  0x12\n')
        self.assertEqual(output, dedent('''\
            [Backtrace #0]
            func_12(char const*) at gib/ber.cc:18

            '''))

    def test_backtrace_continues_after_separator(self):
        """A backtrace continues after a separator line."""
        output = self.resolve(stdin='Backtrace:\n  0x12\n---\n  0x22\n')
        self.assertEqual(output, dedent('''\
            Backtrace:
            [Backtrace #0]
            func_12(char const*) at gib/ber.cc:18
            func_22(char const*) at gib/ber.cc:34

            '''))

    def test_repeated_backtrace(self):
        """A backtrace seen before refers to the first one instead of being resolved again."""
        output = self.resolve(stdin='first: 0x12 0x22\nsecond: 0x12 0x22\n')
        self.assertEqual(output, dedent('''\
            first:
            [Backtrace #0]
            func_12(char const*) at gib/ber.cc:18
            func_22(char const*) at gib/ber.cc:34

            second:
            [Backtrace #0] Already seen, not resolving again.

            '''))

    def test_library_addresses(self):
        """Addresses prefixed by their library are resolved in it; ones in a missing library are printed as they are."""
        output = self.resolve(stdin='Backtrace: 0x12 gibberish+0x22 /nonexistent/lib.so+0x32\n')
        self.assertEqual(output, dedent('''\
            Backtrace:
            [Backtrace #0]
            func_12(char const*) at gib/ber.cc:18
            func_22(char const*) at gib/ber.cc:34
            ''') + '/nonexistent/lib.so 0x32 \n\n')

    def test_sanitizer_backtrace(self):
        """Frames of sanitizer reports are resolved at their offset in the module in parentheses."""
        output = self.resolve(stdin=dedent('''\
            ==1==ERROR: AddressSanitizer: heap-use-after-free on address 0x60700019c710
                #0 0x55d4c0b5e2a2  (gibberish+0x12)
            #1 0x00007fd2dab4f952 abort (gibberish + 0x22)
            '''))
        self.assertEqual(output, dedent('''\
            ==1==ERROR: AddressSanitizer: heap-use-after-free on address 0x60700019c710
            [Backtrace #0]
            func_12(char const*) at gib/ber.cc:18
            func_22(char const*) at gib/ber.cc:34

            '''))

    def test_match_context(self):
        """With --match, only backtraces preceded by a matching line are printed."""
        output = self.resolve('--match', 'wanted',
                              stdin='wanted context\nBacktrace: 0x12 0x22\nother context\nBacktrace: 0x32 0x42\n')
        self.assertEqual(output, dedent('''\
            wanted context
            Backtrace:
            [Backtrace #0]
            func_12(char const*) at gib/ber.cc:18
            func_22(char const*) at gib/ber.cc:34

            '''))

    def test_match_prefix(self):
        """--match also matches the text preceding the addresses of a one-line backtrace."""
        output = self.resolve('--before', '0', '--match', 'wanted',
                              stdin='wanted: 0x12 0x22\nother: 0x32 0x42\n')
        self.assertEqual(output, dedent('''\
            wanted:
            [Backtrace #0]
            func_12(char const*) at gib/ber.cc:18
            func_22(char const*) at gib/ber.cc:34

            '''))

    def test_verbose(self):
        """With --verbose, every frame starts with its module and address."""
        output = self.resolve('--verbose', stdin='Backtrace: 0x12 0x21\n')
        self.assertEqual(output, dedent('''\
            Backtrace:
            [Backtrace #0]
            {gibberish} 0x12: func_12(char const*) at gib/ber.cc:18
            {gibberish} 0x21: inlined_21 at gib/ber.hh:33
             (inlined by) caller_21(int) at gib/ber.cc:33

            '''))

    def test_kernel_addresses(self):
        """Kernel addresses are resolved to the preceding symbol, or to their distance from the symbols outside of them."""
        kallsyms = self.write_file('kallsyms', KALLSYMS)
        output = self.resolve('--kallsyms', kallsyms, stdin='kernel callstack: 0xffffffff81000110'
                              ' 0xffffffff80000000 0xffffffff90000000 0xffffffff81000210\n')
        self.assertEqual(output, 'kernel callstack: \n' + dedent('''\
            [Backtrace #0]
            gibberish_kernel_function+0x10
            0xffffffff80000000 (16777216 bytes before first symbol)
            0xffffffff90000000 (251657728 bytes after last symbol)
            gibberish_last_symbol+0x10

            '''))

    def test_unusable_kallsyms(self):
        """Kernel addresses are printed as they are when the kernel symbols cannot be used."""
        garbage = ''.join(f'garbage line {i}\n' for i in range(1, 12))
        cases = {
            'missing': (None, "Cannot open kallsyms: [Errno 2] No such file or directory: 'kallsyms'\n"),
            'restricted': ('0000000000000000 T _stext\n',
                           'kallsyms is restricted, set /proc/sys/kernel/kptr_restrict to 0 to decode\n'),
            # warnings stop after 10 lines
            'garbage': (garbage, ''.join(f'WARNING: kallsyms regex match failure: garbage line {i}\n'
                                         for i in range(1, 11)) + 'kallsyms was empty\n'),
        }
        for case, (kallsyms, message) in cases.items():
            with self.subTest(case):
                if kallsyms is not None:
                    self.write_file('kallsyms', kallsyms)
                output = self.resolve('--kallsyms', 'kallsyms', stdin=KERNEL_CALLSTACK + '\n')
                self.assertEqual(output, 'kernel callstack: \n[Backtrace #0]\n' + message
                                 + '0xffffffff81000110\n0xffffffff80000000\n\n')

    def test_debug_and_timing(self):
        """--debug and --timing log to stderr, stdout stays the same."""
        res = self.run_script('--executable', EXECUTABLE, '--debug', '--timing',
                              input='Backtrace:\n  0x12\n---\n')
        self.assertEqual(res.returncode, 0, res.stderr)
        self.assertEqual(res.stdout, 'Backtrace:\n[Backtrace #0]\nfunc_12(char const*) at gib/ber.cc:18\n\n')
        self.assertIn('DEBUG >> Addr2line invoking: llvm-addr2line -Cfpia -e gibberish\n', res.stderr)
        self.assertIn('DEBUG >> INPUT LINE [SEPARATOR]: ---\n', res.stderr)
        self.assertIn('seconds :  full resolve loop\n', res.stderr)
        self.assertIn('seconds :  resolve time (addr2line subprocess time)\n', res.stderr)

    def test_executable_without_debug_info(self):
        """An ELF executable without debug info is reported by its file(1) description."""
        (self.workdir / 'stripped').write_bytes(b'\x7fELF' + bytes(60))
        res = self.run_script('--executable', 'stripped', '0x12')
        self.assertEqual(res.returncode, 0, res.stderr)
        self.assertEqual(res.stdout, dedent('''\
            stripped: ELF executable, stripped

            [Backtrace #0]
            func_12(char const*) at gib/ber.cc:18

            '''))

    def test_self_test(self):
        """--test runs the unit tests embedded in the script."""
        res = self.run_script('--test')
        self.assertEqual(res.returncode, 0, res.stderr)
        self.assertIn('OK', res.stderr)

    def test_before_negative(self):
        """With --before -1, all lines are printed."""
        output = self.resolve('--before', '-1', stdin='first line\nBacktrace:\n  0x12\nlast line\n')
        self.assertEqual(output, dedent('''\
            first line
            Backtrace:
            [Backtrace #0]
            func_12(char const*) at gib/ber.cc:18

            last line
            '''))

    def test_context_of_one_backtrace(self):
        """The lines preceding a backtrace are its context only, printed once."""
        output = self.resolve(stdin='context\nfirst: 0x12 0x22\nsecond: 0x32 0x42\n')
        self.assertEqual(output, dedent('''\
            context
            first:
            [Backtrace #0]
            func_12(char const*) at gib/ber.cc:18
            func_22(char const*) at gib/ber.cc:34

            second:
            [Backtrace #1]
            func_32(char const*) at gib/ber.cc:50
            func_42(char const*) at gib/ber.cc:66

            '''))

    def test_match_context_of_one_backtrace(self):
        """--match matches the context of a backtrace only against that backtrace."""
        output = self.resolve('--match', 'wanted', stdin='wanted\nfirst: 0x12 0x22\nsecond: 0x32 0x42\n')
        self.assertEqual(output, dedent('''\
            wanted
            first:
            [Backtrace #0]
            func_12(char const*) at gib/ber.cc:18
            func_22(char const*) at gib/ber.cc:34

            '''))


if __name__ == '__main__':
    unittest.main()
