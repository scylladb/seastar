#!/usr/bin/python3 -I
# Coverage audit for the Seastar book: which public Seastar identifiers do
# ScyllaDB and Redpanda use, and does the book plan (or the book) mention them?
#
# Usage: audit.py SEASTAR_DIR SCYLLA_DIR REDPANDA_DIR BOOK_FILES... > report.txt
# Needs universal-ctags. Prints "scylla_count redpanda_count Y/- name where",
# sorted by usage; "-" means the name does not appear in any BOOK_FILES.

import collections
import json
import re
import subprocess
import sys
from pathlib import Path

seastar, scylla, redpanda = (Path(p) for p in sys.argv[1:4])
book = '\n'.join(Path(f).read_text() for f in sys.argv[4:])

def ctags(kinds):
    out = subprocess.run(['ctags', '-R', '--languages=C++', '--langmap=C++:+.hh',
                          f'--kinds-C++={kinds}', '--fields=+KsZa', '--output-format=json',
                          '-f', '-', str(seastar / 'include/seastar')],
                         capture_output=True, text=True, check=True).stdout
    return [t for t in map(json.loads, out.splitlines()) if t.get('_type') == 'tag']

namespaces = {(t.get('scope', '') + '::' + t['name']).lstrip(':') for t in ctags('n')}
top = collections.defaultdict(set)
for t in ctags('cstfpgdme'):
    scope = t.get('scope', '')
    if 'internal' in scope or '/internal/' in t['path'] or t['name'].startswith('_'):
        continue
    if t['kind'] == 'macro' or scope in namespaces or scope == '':
        top[t['name']].add(f"{t['kind']}:{scope}:{t['path'].split('include/seastar/')[-1]}")

def count(root, patterns):
    c = collections.Counter()
    for pattern in patterns:
        for f in root.glob(pattern):
            if 'seastar/' in str(f.relative_to(root))[:8] or 'thirdparty' in str(f):
                continue
            for w in re.findall(r'[A-Za-z_]\w*', f.read_text(errors='replace')):
                if w in top:
                    c[w] += 1
    return c

sc = count(scylla, ['**/*.cc', '**/*.hh'])
rp = count(redpanda / 'src', ['**/*.cc', '**/*.h'])
rows = sorted(((max(sc[n], rp[n]), sc[n], rp[n], n) for n in top if max(sc[n], rp[n]) >= 3), reverse=True)
for _, a, b, n in rows:
    mentioned = 'Y' if re.search(r'\b' + re.escape(n) + r'\b', book) else '-'
    print(f"{a:6} {b:6} {mentioned} {n:34} {' ; '.join(sorted(top[n]))[:100]}")
