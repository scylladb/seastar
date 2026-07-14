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
Integration test for Seastar IO tracing via LTTng-UST.

This script:
1. Creates an LTTng userspace tracing session
2. Enables seastar_io:* events with procname context
3. Runs io_tester with a config that produces distinct IO patterns per shard:
   - Shard 0: sequential reads, 4kB requests
   - Shard 1: random writes, 16kB requests
4. Stops the session and reads back events via babeltrace2
5. Verifies that:
   - All expected tracepoint types fired (queued, dispatched, completed)
   - Event ordering is correct (queued → dispatched → completed)
   - Per-shard separation works: shard 0 events are reads with length=4096,
     shard 1 events are writes with length=16384
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile


# io_tester YAML config: distinct jobs on distinct shards.
# Shard 0: seqread 4kB, shard 1: overwrite 16kB.
IO_TESTER_CONF = """\
- name: shard0_reads
  type: seqread
  shards: [0]
  shard_info:
    parallelism: 4
    reqsize: 4kB
    shares: 100

- name: shard1_writes
  type: overwrite
  shards: [1]
  shard_info:
    parallelism: 4
    reqsize: 16kB
    shares: 100
"""


def run(cmd, check=True, capture=False):
    """Run a command, optionally capturing output."""
    result = subprocess.run(cmd, capture_output=capture, text=True)
    if check and result.returncode != 0:
        print(f"Command failed: {' '.join(cmd)}", file=sys.stderr)
        if result.stderr:
            print(result.stderr, file=sys.stderr)
        sys.exit(1)
    return result


def check_prerequisites():
    """Verify that lttng tools and babeltrace2 are available."""
    for tool in ['lttng', 'babeltrace2']:
        if not shutil.which(tool):
            print(f"SKIP: '{tool}' not found in PATH", file=sys.stderr)
            sys.exit(0)

    # Check if lttng-sessiond is running (or can be started)
    result = subprocess.run(['lttng', 'list', '-u'], capture_output=True, text=True)
    if result.returncode != 0:
        result = subprocess.run(['lttng-sessiond', '--daemonize'],
                                capture_output=True, text=True)
        if result.returncode != 0:
            print("SKIP: cannot start lttng-sessiond", file=sys.stderr)
            sys.exit(0)


def parse_queued_event(event_line):
    """Parse an io_queue_queued event line and return a dict of fields."""
    fields = {}
    # Extract direction, length, procname from the event line
    if 'direction = ' in event_line:
        fields['direction'] = int(event_line.split('direction = ')[1].split(',')[0].split('}')[0])
    if 'length = ' in event_line:
        fields['length'] = int(event_line.split('length = ')[1].split(',')[0].split('}')[0])
    if 'procname = "' in event_line:
        fields['procname'] = event_line.split('procname = "')[1].split('"')[0]
    if 'req_id = ' in event_line:
        fields['req_id'] = event_line.split('req_id = ')[1].split(',')[0].split('}')[0].strip()
    return fields


def main():
    parser = argparse.ArgumentParser(description='IO tracing integration test')
    parser.add_argument('--io-tester', required=True,
                        help='Path to the io_tester binary')
    args = parser.parse_args()

    if not os.path.isfile(args.io_tester):
        print(f"ERROR: binary not found: {args.io_tester}", file=sys.stderr)
        sys.exit(1)

    check_prerequisites()

    session_name = 'seastar-io-trace-test'
    trace_dir = tempfile.mkdtemp(prefix='seastar-io-trace-')
    conf_dir = tempfile.mkdtemp(prefix='seastar-io-trace-conf-')

    try:
        # Write io_tester config
        conf_path = os.path.join(conf_dir, 'conf.yaml')
        with open(conf_path, 'w') as f:
            f.write(IO_TESTER_CONF)

        # Destroy any leftover session from a previous failed run
        subprocess.run(['lttng', 'destroy', session_name],
                       capture_output=True, text=True)

        # Create session
        run(['lttng', 'create', session_name, '--output', trace_dir])

        # Enable all seastar_io tracepoints
        run(['lttng', 'enable-event', '-u', 'seastar_io:*'])

        # Add thread context for per-shard identification
        run(['lttng', 'add-context', '-u', '-t', 'vtid', '-t', 'procname'])

        # Start recording
        run(['lttng', 'start'])

        # Run io_tester: 2 shards, short duration, our custom config.
        # --storage /dev/null avoids real file creation; IO still goes
        # through the IO scheduler and triggers tracepoints.
        result = subprocess.run(
            [args.io_tester,
             '--conf', conf_path,
             '--storage', '/dev/null',
             '--duration', '1',
             '-c2'],
            capture_output=True, text=True, timeout=30)
        if result.returncode != 0:
            print(f"io_tester failed:\n{result.stdout}\n{result.stderr}",
                  file=sys.stderr)
            sys.exit(1)

        # Stop recording
        run(['lttng', 'stop'])

        # Read events via babeltrace2
        bt_result = run(['babeltrace2', trace_dir], capture=True)
        events = bt_result.stdout.strip().split('\n')
        events = [e for e in events if e]

        # --- Check 1: all expected event types present ---
        event_types = set()
        for event in events:
            if 'seastar_io:io_queue_queued' in event:
                event_types.add('queued')
            elif 'seastar_io:io_queue_dispatched' in event:
                event_types.add('dispatched')
            elif 'seastar_io:io_queue_completed' in event:
                event_types.add('completed')

        expected = {'queued', 'dispatched', 'completed'}
        missing = expected - event_types
        if missing:
            print(f"FAIL: missing expected event types: {missing}",
                  file=sys.stderr)
            print(f"Raw output ({len(events)} lines):", file=sys.stderr)
            for e in events[:20]:
                print(f"  {e}", file=sys.stderr)
            sys.exit(1)

        # --- Check 2: event ordering (queued → dispatched → completed) ---
        req_events = {}
        for event in events:
            if 'seastar_io:' not in event:
                continue
            parts = event.split('req_id = ')
            if len(parts) < 2:
                continue
            req_id = parts[1].split(',')[0].split('}')[0].strip()
            if 'io_queue_queued' in event:
                req_events.setdefault(req_id, []).append('queued')
            elif 'io_queue_dispatched' in event:
                req_events.setdefault(req_id, []).append('dispatched')
            elif 'io_queue_completed' in event:
                req_events.setdefault(req_id, []).append('completed')

        for req_id, seq in req_events.items():
            if seq == ['queued', 'dispatched', 'completed']:
                continue
            if 'dispatched' in seq and 'queued' not in seq:
                print(f"FAIL: req {req_id} has dispatched without queued: {seq}",
                      file=sys.stderr)
                sys.exit(1)
            if 'completed' in seq and 'dispatched' not in seq:
                print(f"FAIL: req {req_id} has completed without dispatched: {seq}",
                      file=sys.stderr)
                sys.exit(1)

        print(f"PASS: captured {len(events)} events, "
              f"{len(req_events)} unique requests, "
              f"types: {sorted(event_types)}")

        # --- Check 3: per-shard separation with distinct patterns ---
        # Shard 0 (procname = binary name truncated to 15 chars): reads (direction=1), length=4096
        # Shard 1 (procname = "reactor-1"): writes (direction=0), length=16384
        shard0_procname = os.path.basename(args.io_tester)[:15]
        shard0_events = []
        shard1_events = []

        for event in events:
            if 'seastar_io:io_queue_queued' not in event:
                continue
            fields = parse_queued_event(event)
            pname = fields.get('procname', '')
            if pname == 'reactor-1':
                shard1_events.append(fields)
            elif pname == shard0_procname:
                shard0_events.append(fields)

        if not shard0_events:
            print("FAIL: no queued events found for shard 0", file=sys.stderr)
            sys.exit(1)
        if not shard1_events:
            print("FAIL: no queued events found for shard 1", file=sys.stderr)
            sys.exit(1)

        # Verify shard 0: reads (direction=1), length=4096
        for ev in shard0_events:
            if ev.get('direction') != 1:
                print(f"FAIL: shard 0 event has direction={ev.get('direction')}, "
                      f"expected 1 (read)", file=sys.stderr)
                sys.exit(1)
            if ev.get('length') != 4096:
                print(f"FAIL: shard 0 event has length={ev.get('length')}, "
                      f"expected 4096", file=sys.stderr)
                sys.exit(1)

        # Verify shard 1: writes (direction=0), length=16384
        for ev in shard1_events:
            if ev.get('direction') != 0:
                print(f"FAIL: shard 1 event has direction={ev.get('direction')}, "
                      f"expected 0 (write)", file=sys.stderr)
                sys.exit(1)
            if ev.get('length') != 16384:
                print(f"FAIL: shard 1 event has length={ev.get('length')}, "
                      f"expected 16384", file=sys.stderr)
                sys.exit(1)

        print(f"PASS: per-shard separation verified — "
              f"shard 0: {len(shard0_events)} reads (4kB), "
              f"shard 1: {len(shard1_events)} writes (16kB)")

    finally:
        # Cleanup
        subprocess.run(['lttng', 'destroy', session_name],
                       capture_output=True, text=True)
        shutil.rmtree(trace_dir, ignore_errors=True)
        shutil.rmtree(conf_dir, ignore_errors=True)


if __name__ == '__main__':
    main()
