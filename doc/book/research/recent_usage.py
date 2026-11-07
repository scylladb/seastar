#!/usr/bin/python3 -I
# Which Seastar APIs does recent ScyllaDB development use, compared with
# ScyllaDB's code as a whole? Counts each public Seastar identifier (and some
# key member functions and keywords) in:
#   - the current tree (C++ files at REF),
#   - lines added and removed by non-merge commits since SINCE.
# Code that is only moved is both added and removed, so "net" (added minus
# removed) shows what new code really adds.
#
# Usage: recent_usage.py SEASTAR_DIR SCYLLA_GIT_DIR REF SINCE > report.tsv
# Example: recent_usage.py ~/seastar ~/scylla origin/master 2025-11-05

import collections
import json
import re
import subprocess
import sys
import tarfile
from pathlib import Path

seastar, scylla, ref, since = Path(sys.argv[1]), sys.argv[2], sys.argv[3], sys.argv[4]

def ctags(kinds):
    out = subprocess.run(['ctags', '-R', '--languages=C++', '--langmap=C++:+.hh',
                          f'--kinds-C++={kinds}', '--fields=+KsZa', '--output-format=json',
                          '-f', '-', str(seastar / 'include/seastar')],
                         capture_output=True, text=True, check=True).stdout
    return [t for t in map(json.loads, out.splitlines()) if t.get('_type') == 'tag']

# Namespace-level public identifiers, chosen as in audit.py.
namespaces = {(t.get('scope', '') + '::' + t['name']).lstrip(':') for t in ctags('n')}
names = set()
for t in ctags('cstfpgdme'):
    scope = t.get('scope', '')
    if 'internal' in scope or '/internal/' in t['path'] or t['name'].startswith('_'):
        continue
    if t['kind'] == 'macro' or scope in namespaces or scope == '':
        names.add(t['name'])

# Member functions worth tracking, counted only in call syntax (.f( or ->f().
members = ['then', 'then_wrapped', 'finally', 'handle_exception', 'handle_exception_type',
           'discard_result', 'or_terminate', 'get', 'invoke_on', 'invoke_on_all',
           'invoke_on_others', 'map_reduce0', 'local', 'container', 'hold', 'close',
           'abort_requested', 'check', 'request_abort', 'subscribe', 'wait', 'signal',
           'broken', 'with_lock', 'hold_read_lock', 'hold_write_lock', 'arm', 'cancel',
           'get_future', 'set_value', 'set_exception', 'available', 'failed']
keywords = ['co_await', 'co_return']
# Qualified names, counted as literal text, to tell apart APIs whose short names
# are ambiguous (for example coroutine::parallel_for_each vs. parallel_for_each).
phrases = ['coroutine::parallel_for_each', 'coroutine::all', 'coroutine::maybe_yield',
           'coroutine::exception', 'coroutine::return_exception', 'coroutine::as_future',
           'coroutine::try_future', 'coroutine::switch_to', 'coroutine::lambda',
           'coroutine::generator', 'seastar::async', 'smp::submit_to', 'smp::count',
           'this_smp_shard_count()', 'this_shard_id()', 'do_with(', 'futurize_invoke(',
           'when_all(', 'when_all_succeed(', 'max_concurrent_for_each(', 'repeat(',
           'do_until(', 'do_for_each(', 'keep_doing(', 'with_gate(', 'get_units(',
           'with_semaphore(', 'with_scheduling_group(', 'make_lw_shared', 'make_foreign(',
           'make_ready_future', 'make_exception_future', 'current_exception_as_future',
           'SEASTAR_TEST_CASE(', 'SEASTAR_THREAD_TEST_CASE(', 'SEASTAR_FIXTURE_TEST_CASE(',
           'yield()', 'maybe_yield()', '(this auto', 'sharded<', 'foreign_ptr<',
           'lw_shared_ptr<', 'shared_ptr<', 'chunked_fifo<', 'circular_buffer<',
           'noncopyable_function<', 'abort_source', 'condition_variable', 'named_semaphore',
           'rwlock', 'shared_mutex', 'deferred_close', 'deferred_stop', 'defer(',
           'lowres_clock', 'manual_clock', 'open_file_dma', 'input_stream<', 'output_stream<',
           'temporary_buffer<', 'sstring', 'scheduling_group', 'thread::maybe_yield',
           'thread::should_yield', 'seastar::thread', 'json_return_type', 'httpd::',
           'rpc::', 'metrics::', 'gate::holder', 'optimized_optional<']

word = re.compile(r'[A-Za-z_]\w*')
member_call = re.compile(r'(?:\.|->)(' + '|'.join(members) + r')\s*[(<]')

def count(text, c, sign=1):
    for w in word.findall(text):
        if w in names or w in keywords:
            c[w] += sign
    for m in member_call.findall(text):
        c['.' + m + '()'] += sign
    for ph in phrases:
        if ph in text:
            c['"' + ph + '"'] += sign * text.count(ph)

def cpp(path):
    return (path.endswith('.cc') or path.endswith('.hh')) and not path.startswith('seastar/')

# The current tree.
current = collections.Counter()
archive = subprocess.Popen(['git', '-C', scylla, 'archive', ref], stdout=subprocess.PIPE)
with tarfile.open(fileobj=archive.stdout, mode='r|') as tar:
    for member in tar:
        if member.isfile() and cpp(member.name):
            count(tar.extractfile(member).read().decode(errors='replace'), current)
archive.wait()

# Lines added and removed since SINCE.
added, removed = collections.Counter(), collections.Counter()
log = subprocess.Popen(['git', '-C', scylla, 'log', ref, f'--since={since}', '--no-merges',
                        '-p', '--unified=0', '--format=', '--', '*.cc', '*.hh', ':!seastar'],
                       stdout=subprocess.PIPE, text=True, errors='replace')
for line in log.stdout:
    if line.startswith('+') and not line.startswith('+++'):
        count(line[1:], added)
    elif line.startswith('-') and not line.startswith('---'):
        count(line[1:], removed)
log.wait()

total_current = sum(current.values())
total_added = sum(added.values())
print('name\tcurrent\tadded\tremoved\tnet\tratio')
for n in sorted(set(current) | set(added), key=lambda n: -(current[n] + added[n])):
    # ratio > 1: the name is more common in last year's additions than in the code as a whole.
    ratio = (added[n] / total_added) / (current[n] / total_current) if current[n] else float('inf')
    print(f'{n}\t{current[n]}\t{added[n]}\t{removed[n]}\t{added[n] - removed[n]}\t{ratio:.2f}')
