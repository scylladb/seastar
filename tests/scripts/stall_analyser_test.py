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
End-to-end tests of scripts/stall-analyser.py.

Each test runs the analyser on a few stall reports written by stall() and
checks the printed output. Addresses get resolved by the fake
fake-bin/llvm-addr2line in the made-up executable EXECUTABLE:
  0x...0 -> ?? at ??:0
  0x...1 -> inlined_<addr> at gib/ber.hh:<addr % 97>
            (inlined by) caller_<addr>(int) at gib/ber.cc:<addr % 89>
  others -> func_<addr>(char const*) at gib/ber.cc:<addr % 101>
Unless --full-function-names is given, the fake fake-bin/c++filt drops the
parameter lists, e.g. func_12(char const*) becomes func_12.
"""

import os
import unittest
from textwrap import dedent

from testcase import EXECUTABLE, ScriptTestCase


def stall(ms: int, *addresses: str) -> str:
    """Returns a stall report of the given duration, innermost frame first."""
    return f'Reactor stalled for {ms} ms on shard 0. Backtrace: {" ".join(addresses)}\n'


def graph(output: str) -> str:
    """Returns the graph part of the analyser output, which follows the header and the legend."""
    legend = output.index('  pct    - ')
    return output[output.index('\n\n', legend) + 2:]


class TestStallAnalyser(ScriptTestCase):
    """Runs stall-analyser.py on stall reports and checks its output."""

    SCRIPT = 'stall-analyser.py'

    def analyse(self, *args: str, stdin: str) -> str:
        """Runs the analyser with the given arguments and stdin, checks it succeeded and returns its stdout."""
        res = self.run_script(*args, input=stdin)
        self.assertEqual(res.returncode, 0, res.stderr)
        self.assertEqual(res.stderr, '')
        return res.stdout

    def test_header(self):
        """The output starts with the command line options, followed by the stall statistics and the graph."""
        output = self.analyse('--use-debuginfod', stdin=stall(100, '0x12'))
        options, stats, legend = output.split('\n\n')[:3]
        self.assertTrue(options.startswith('Command line options: '))
        self.assertIn(' --use-debuginfod ', options)
        self.assertTrue(stats.startswith('Processed 1 stalls lasting a total of 100 milliseconds.\n'))
        self.assertTrue(legend.startswith('This graph is printed in bottom-up order, where callees are printed first.\n'))

    def test_percentiles(self):
        """The statistics show the median and the 95th, 99th and 99.9th percentile of the stall durations."""
        stalls = (50 * stall(10, '0x12') + 45 * stall(20, '0x12') + 4 * stall(30, '0x12')
                  + stall(40, '0x12'))
        output = self.analyse(stdin=stalls)
        self.assertIn('Processed 100 stalls lasting a total of 1560 milliseconds.\n'
                      'min=10 avg=15.6 median=10 p95=20 p99=30 p999=40 max=40\n', output)

    def test_minimum(self):
        """With --minimum, the graph contains only the stalls at least that long."""
        output = self.analyse('--minimum', '20', stdin=stall(10, '0x12') + stall(50, '0x22'))
        self.assertIn('Processed 2 stalls lasting a total of 60 milliseconds.\n'
                      'Of which, 1 lasted 20 milliseconds or longer.\n', output)
        self.assertEqual(graph(output), dedent('''\
            ++[0#1/1 100%] addr=0x22 total=50 count=1 avg=50
            '''))

    def test_only_stall_reports_analysed(self):
        """Only the stall reports outside of comment lines are analysed."""
        output = self.analyse(stdin='# ' + stall(1, '0x12') + 'some other line\n' + stall(100, '0x22'))
        self.assertIn('Processed 1 stalls lasting a total of 100 milliseconds.\n', output)

    def test_frames_end_at_build_id(self):
        """The frames of a backtrace end at the build id following it."""
        output = self.analyse(stdin='Reactor stalled for 100 ms on shard 0. Backtrace: 0x12 0x22'
                                    ' (BuildId: 0123abcd)\n')
        self.assertEqual(graph(output), dedent('''\
            ++[0#1/1 100%] addr=0x12 total=100 count=1 avg=100
            ++           - addr=0x22
            '''))

    def test_file_argument(self):
        """The stall reports are read from the file given as argument."""
        log = self.write_file('stalls.log', stall(100, '0x12'))
        output = self.analyse(log, stdin='')
        self.assertIn('Processed 1 stalls lasting a total of 100 milliseconds.\n', output)

    def test_chain_without_branches(self):
        """A chain of single callers shows its statistics once, on its first frame."""
        output = self.analyse(stdin=stall(100, '0x12', '0x22', '0x32'))
        self.assertEqual(graph(output), dedent('''\
            ++[0#1/1 100%] addr=0x12 total=100 count=1 avg=100
            ++           - addr=0x22
            ++           - addr=0x32
            '''))

    def test_siblings_sorted_by_total(self):
        """Siblings are printed by descending total stall time, with their share of it."""
        output = self.analyse(stdin=stall(100, '0x12') + stall(300, '0x22'))
        self.assertEqual(graph(output), dedent('''\
            ++[0#1/2 75%] addr=0x22 total=300 count=1 avg=300
            ++[0#2/2 25%] addr=0x12 total=100 count=1 avg=100
            '''))

    def test_node_printed_once(self):
        """A node reached again refers to its first appearance."""
        output = self.analyse(stdin=stall(100, '0x12', '0x32') + stall(200, '0x22', '0x32'))
        self.assertEqual(graph(output), dedent('''\
            ++[0#1/2 67%] addr=0x22 total=200 count=1 avg=200
            ++          - addr=0x32
            ++[0#2/2 33%] addr=0x12 total=100 count=1 avg=100
            ++          - addr=0x32
            -> continued at addr=0x32 above
            '''))

    def test_longer_backtrace_extends_chain(self):
        """A backtrace extending a previous one continues its chain."""
        output = self.analyse(stdin=stall(100, '0x12', '0x22') + stall(50, '0x12', '0x22', '0x32'))
        self.assertEqual(graph(output), dedent('''\
            ++[0#1/1 100%] addr=0x12 total=150 count=2 avg=75
            ++           - addr=0x22
              ++[1#1/1 100%] addr=0x32 total=50 count=1 avg=50
            '''))

    def test_top_down(self):
        """With --direction=top-down, the outermost callers are printed first."""
        output = self.analyse('--direction', 'top-down', stdin=stall(100, '0x12', '0x22'))
        self.assertIn('This graph is printed in top-down order, where callers are printed first.\n',
                      output)
        self.assertEqual(graph(output), dedent('''\
            ++[0#1/1 100%] addr=0x22 total=100 count=1 avg=100
            ++           - addr=0x12
            '''))

    def test_small_branches_summarised(self):
        """Branches below 3% of their siblings' total are summarised in one line."""
        output = self.analyse(stdin=stall(100, '0x12', '0x32') + stall(1, '0x12', '0x42'))
        self.assertEqual(graph(output), dedent('''\
            ++[0#1/1 100%] addr=0x12 total=101 count=2 avg=50
              ++[1#1/2 99%] addr=0x32 total=100 count=1 avg=100
              ++[1#2/2 1%] 1 more branches total=1 count=1 avg=1
            '''))

    def test_branch_threshold(self):
        """--branch-threshold sets the share below which branches get summarised."""
        output = self.analyse('--branch-threshold', '0.3',
                              stdin=stall(100, '0x12', '0x32') + stall(20, '0x12', '0x42'))
        self.assertEqual(graph(output), dedent('''\
            ++[0#1/1 100%] addr=0x12 total=120 count=2 avg=60
              ++[1#1/2 83%] addr=0x32 total=100 count=1 avg=100
              ++[1#2/2 17%] 1 more branches total=20 count=1 avg=20
            '''))

    def test_address_threshold(self):
        """Frames up to and including the first one at or above --address-threshold, 0x100000000 by default,
        are dropped; 0 keeps all frames."""
        stalls = stall(100, '0x7f0000000010', '0x12')
        self.assertEqual(graph(self.analyse(stdin=stalls)), dedent('''\
            ++[0#1/1 100%] addr=0x12 total=100 count=1 avg=100
            '''))
        self.assertEqual(graph(self.analyse('--address-threshold', '0', stdin=stalls)), dedent('''\
            ++[0#1/1 100%] addr=0x7f0000000010 total=100 count=1 avg=100
            ++           - addr=0x12
            '''))

    def test_width(self):
        """--width trims the printed lines."""
        output = self.analyse('--width', '20', stdin=stall(100, '0x12', '0x22'))
        self.assertEqual(graph(output), dedent('''\
            ++[0#1/1 100%] addr=
            ++           - addr=
            '''))

    def test_resolved(self):
        """With --executable, frames are followed by their function names and source locations,
        inlined ones on several lines."""
        output = self.analyse('--executable', EXECUTABLE, stdin=stall(100, '0x12', '0x21', '0x30'))
        self.assertEqual(graph(output), dedent('''\
            ++[0#1/1 100%] addr=0x12 total=100 count=1 avg=100:
            |              func_12 at gib/ber.cc:18
            ++           - addr=0x21:
            |              inlined_21 at gib/ber.hh:33
            |              (inlined by) caller_21 at gib/ber.cc:33
            ++           - addr=0x30: ?? at ??:0
            '''))

    def test_resolved_top_down(self):
        """Top-down, the inlined frames of an address are printed outermost first."""
        output = self.analyse('--executable', EXECUTABLE, '--direction', 'top-down',
                              stdin=stall(100, '0x21'))
        self.assertEqual(graph(output), dedent('''\
            ++[0#1/1 100%] addr=0x21 total=100 count=1 avg=100:
            |              (inlined by) caller_21 at gib/ber.cc:33
            |              inlined_21 at gib/ber.hh:33
            '''))

    def test_full_function_names(self):
        """With --full-function-names, functions are printed with their parameters."""
        output = self.analyse('--executable', EXECUTABLE, '--full-function-names',
                              stdin=stall(100, '0x12', '0x21'))
        self.assertEqual(graph(output), dedent('''\
            ++[0#1/1 100%] addr=0x12 total=100 count=1 avg=100:
            |              func_12(char const*) at gib/ber.cc:18
            ++           - addr=0x21:
            |              inlined_21 at gib/ber.hh:33
            |              (inlined by) caller_21(int) at gib/ber.cc:33
            '''))

    def test_resolved_width(self):
        """--width trims resolved lines before their source location."""
        output = self.analyse('--executable', EXECUTABLE, '--width', '30', stdin=stall(100, '0x12'))
        self.assertEqual(graph(output), dedent('''\
            ++[0#1/1 100%] addr=0x12 total
            |         ... at gib/ber.cc:18
            '''))

    def test_resolved_width_shorter_than_location(self):
        """--width shorter than the source location keeps only the source location."""
        output = self.analyse('--executable', EXECUTABLE, '--width', '10', stdin=stall(100, '0x12'))
        self.assertEqual(graph(output), dedent('''\
            ++[0#1/1 1
            ... at gib/ber.cc:18
            '''))

    def test_library_addresses_resolved(self):
        """Addresses prefixed by their library are resolved in that library."""
        output = self.analyse('--executable', EXECUTABLE, '--address-threshold', '0',
                              stdin=stall(100, '0x12', 'gibberish+0x31'))
        self.assertEqual(graph(output), dedent('''\
            ++[0#1/1 100%] addr=0x12 total=100 count=1 avg=100:
            |              func_12 at gib/ber.cc:18
            ++           - addr=gibberish+0x31:
            |              inlined_31 at gib/ber.hh:49
            |              (inlined by) caller_31 at gib/ber.cc:49
            '''))

    def test_trace_format(self):
        """--format=trace prints each distinct backtrace on one line, outermost frame first,
        with the total stall time; inlined frames are marked with _[i], unknown ones left out."""
        output = self.analyse('--executable', EXECUTABLE, '--format', 'trace',
                              stdin=2 * stall(100, '0x12', '0x21', '0x30') + stall(50, '0x22'))
        self.assertEqual(output, dedent('''\
            caller_21_[i];inlined_21;func_12 200
            func_22 50
            '''))

    def test_no_input(self):
        """Input without any stall report fails."""
        res = self.run_script(input='no stall reports here\n')
        self.assertEqual(res.returncode, 1)
        self.assertIn('No input data found.', res.stderr)

    def test_no_backtrace(self):
        """A stall report without frames below the address threshold fails."""
        line = 'Reactor stalled for 10 ms on shard 0. Backtrace: 0x7f0000000010'
        res = self.run_script(input=line + '\n')
        self.assertEqual(res.returncode, 1)
        self.assertIn(f"Invalid input line: '{line}'", res.stderr)

    def test_empty_backtrace(self):
        """A stall report without addresses is ignored below --minimum and fails otherwise."""
        line = 'Reactor stalled for 10 ms on shard 0. Backtrace:'
        output = self.analyse('--minimum', '20', stdin=line + '\n' + stall(50, '0x22'))
        self.assertIn('Of which, 1 lasted 20 milliseconds or longer.\n', output)
        res = self.run_script('--minimum', '5', input=line + '\n')
        self.assertEqual(res.returncode, 1)
        self.assertIn(f"Invalid input line: '{line}'", res.stderr)
        self.assertNotIn('Traceback', res.stderr)

    def test_broken_pipe(self):
        """The analyser exits successfully when its stdout reader goes away."""
        read_end, write_end = os.pipe()
        os.close(read_end)
        try:
            res = self.run_script(input=stall(100, '0x12'), stdout=write_end)
        finally:
            os.close(write_end)
        self.assertEqual(res.returncode, 0, res.stderr)
        self.assertNotIn('Traceback', res.stderr)

    def test_trampoline_in_library(self):
        """Frames up to and including the first one in a library are dropped."""
        output = self.analyse(stdin=stall(100, '0x22', 'libc.so.6+0x3e6f0', '0x12'))
        self.assertEqual(graph(output), dedent('''\
            ++[0#1/1 100%] addr=0x12 total=100 count=1 avg=100
            '''))

    def test_innermost_frame_seen_as_caller(self):
        """A backtrace keeps its stall time after its innermost frame is seen as a caller."""
        output = self.analyse(stdin=stall(100, '0x22') + stall(50, '0x12', '0x22'))
        self.assertEqual(graph(output), dedent('''\
            ++[0#1/2 67%] addr=0x22 total=100 count=1 avg=100
            ++[0#2/2 33%] addr=0x12 total=50 count=1 avg=50
            ++          - addr=0x22
            -> continued at addr=0x22 above
            '''))

    def test_top_down_outermost_frame_seen_as_callee(self):
        """Top-down, a backtrace keeps its stall time after its outermost frame is seen as a callee."""
        output = self.analyse('--direction', 'top-down',
                              stdin=stall(100, '0x12', '0x22') + stall(50, '0x12', '0x22', '0x32'))
        self.assertEqual(graph(output), dedent('''\
            ++[0#1/2 67%] addr=0x22 total=100 count=1 avg=100
            | ++[1#1/1 100%] addr=0x12 total=150 count=2 avg=75
            ++[0#2/2 33%] addr=0x32 total=50 count=1 avg=50
            ++          - addr=0x22
            -> continued at addr=0x22 above
            '''))

    def test_frames_after_trampoline(self):
        """Library frames following the trampoline, e.g. of a stall in libc, are kept."""
        output = self.analyse(stdin=stall(100, '0x12', 'libc.so.6+0x3e6f0', 'libc.so.6+0x1a2b3', '0x22'))
        self.assertEqual(graph(output), dedent('''\
            ++[0#1/1 100%] addr=libc.so.6+0x1a2b3 total=100 count=1 avg=100
            ++           - addr=0x22
            '''))

    def test_shared_seastar(self):
        """With Seastar in a shared library, the trampoline is the first frame in another library."""
        output = self.analyse(stdin=stall(100, 'libseastar.so+0x12', 'libseastar.so+0x22',
                                          'libc.so.6+0x3e6f0', 'libseastar.so+0x32'))
        self.assertEqual(graph(output), dedent('''\
            ++[0#1/1 100%] addr=libseastar.so+0x32 total=100 count=1 avg=100
            '''))

    def test_stall_detector_frames_dropped(self):
        """Frames up to and including the trampoline are dropped, wherever it is."""
        output = self.analyse(stdin=stall(100, '0x12', '0x32', '0x7f0000000010', '0x22'))
        self.assertEqual(graph(output), dedent('''\
            ++[0#1/1 100%] addr=0x22 total=100 count=1 avg=100
            '''))

    def test_recursion(self):
        """A recursive call counts the stall time of its backtrace once."""
        output = self.analyse(stdin=stall(100, '0x12', '0x12', '0x12'))
        self.assertEqual(graph(output), dedent('''\
            ++[0#1/1 100%] addr=0x12 total=100 count=1 avg=100
            ++           - addr=0x12
            -> continued at addr=0x12 above
            '''))


if __name__ == '__main__':
    unittest.main()
