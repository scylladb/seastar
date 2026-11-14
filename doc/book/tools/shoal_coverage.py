#!/usr/bin/python3 -I
# Shows which parts of each Shoal version's source the book never shows:
# lines outside every snippet that a chapter includes, skipping blank lines,
# comments, #include lines and the snippet markers themselves. A hidden line
# that a chapter showed for an earlier version, unchanged, is not listed.
# Use it to check that every piece of Shoal is either shown or deliberately
# left out ("not shown" in the text).
#
# Usage: shoal_coverage.py [VERSION...]   (default: all, such as v0 v1 v2)

import re
import sys
from pathlib import Path

book = Path(__file__).resolve().parent.parent
marker = re.compile(r'^\s*(?://|#) (end )?snippet: ([\w-]+)\s*$')
include = re.compile(r'include="(shoal/[^"]+)"(?:\s+snippet="([\w-]+)")?')

shown = {}   # file -> set of snippet names (None: the whole file)
for chapter in (book / 'chapters').glob('*.md'):
    for m in include.finditer(chapter.read_text()):
        shown.setdefault(m.group(1), set()).add(m.group(2))

def shown_lines(rel):
    """The lines of a Shoal file that the book shows."""
    lines = (book / rel).read_text().splitlines()
    names = shown.get(rel, set())
    if None in names:
        return set(lines)
    result, active = set(), []
    for line in lines:
        m = marker.match(line)
        if m:
            if m.group(1):
                active.remove(m.group(2))
            else:
                active.append(m.group(2))
        elif any(a in names for a in active):
            result.add(line.strip())
    return result

def trivial(line):
    s = line.strip()
    return (not s or s.startswith('//') or s.startswith('#include') or s.startswith('#pragma')
            or marker.match(line) or s in ('{', '}', '};', 'public:', 'private:'))

versions = sorted((d.name for d in (book / 'shoal').glob('v*')),
                  key=lambda v: tuple(int(n) for n in v[1:].split('.')))
wanted = sys.argv[1:] or versions
seen_before = set()     # lines shown for earlier versions
for version in versions:
    files = sorted(p for p in (book / 'shoal' / version).iterdir()
                   if p.is_file() and (p.suffix in ('.cc', '.hh') or p.name == 'CMakeLists.txt'))
    shown_now = set()
    report = []
    for f in files:
        rel = f'shoal/{version}/{f.name}'
        lines_shown = shown_lines(rel)
        shown_now |= lines_shown
        hidden = [(i + 1, line) for i, line in enumerate(f.read_text().splitlines())
                  if not trivial(line) and line.strip() not in lines_shown
                  and line.strip() not in seen_before]
        if hidden:
            report.append((rel, hidden))
    seen_before |= shown_now
    if version not in wanted:
        continue
    print(f'== {version}: {sum(len(h) for _, h in report)} hidden lines')
    for rel, hidden in report:
        print(f'  {rel}:')
        # Print runs of consecutive lines as ranges, with the first line.
        start = prev = None
        for n, line in hidden + [(None, None)]:
            if start is not None and n != prev + 1:
                first = dict(hidden)[start].strip()
                print(f'    {start}-{prev}: {first[:70]}' if prev != start else f'    {start}: {first[:70]}')
                start = None
            if n is not None and start is None:
                start = n
            prev = n
