#!/usr/bin/python3 -I
# Lists the Seastar names whose header the book doesn't name where it
# introduces them. The rule (see the README's "Code"): where the text
# mainly explains a Seastar class, function, macro or type, the paragraph
# says which header it comes from, as in "`handle_signal()`, from
# `<seastar/core/signal.hh>`". The place is the name's index entry with
# .main, or, if it has none, its first mention in the book. Members of
# classes and namespaces are not checked: they come with their class, or
# with what's in them.
#
# A paragraph passes if it names any Seastar header (a paragraph that
# introduces several names from one header names it once), and so does a
# name whose header the book has named already, by then: for another name
# from it, or in the #include lines of a whole file that it shows.
# A name that the book only mentions in passing, without a .main entry,
# passes if its header is named anywhere. Chapter 1 shows no code, so
# names count from their first mention after it.
#
# Usage: header_mentions.py [CHAPTER...]   (default: all chapters, in order)

import re
import subprocess
import sys
from pathlib import Path

book = Path(__file__).resolve().parent.parent
# As in bookfilter.py, which can only run as Pandoc's filter.
seastar_names = dict(line.split(' ', 1)[::-1] for line in
                     (book / 'tools' / 'seastar_names.txt').read_text().splitlines()
                     if line and not line.startswith('#'))
code_name = re.compile(r'^(?:seastar::)?([A-Za-z_]\w*(?:::[A-Za-z_]\w*)*)(?:<[^()]*>)?(?:\(.*\))?$')
all_chapters = [line.strip()[2:] for line in (book / 'metadata.yaml').read_text().splitlines()
                if re.match(r'\s+- [a-z0-9-]+$', line)]
chapters = sys.argv[1:] or all_chapters

def resolve(text, explicit):
    """The full Seastar name that a code span or index entry names, if it's
    one this tool checks, or None."""
    m = code_name.match(text)
    if not m:
        return None
    name = m.group(1)
    full = name if name.startswith('SEASTAR_') and name in seastar_names else 'seastar::' + name
    kind = seastar_names.get(full)
    if kind not in ('class', 'function', 'macro', 'type'):
        return None
    if kind == 'function' and '(' not in text and not (explicit or text.startswith('seastar::')):
        return None
    parent = full.rsplit('::', 1)[0]
    if '::' in full and seastar_names.get(parent) == 'class':
        return None
    return full

header = re.compile(r'<seastar/[\w/.-]+\.hh>')
span = re.compile(r'\[([^\]]*)\]\{([^}]*)\}|`([^`\n]+)`(?!\{)')

# The first mention and the main place of every name: (chapter, paragraph).
first = {}
main = {}
whole_file_includes = {}
for chapter in all_chapters:
    if chapter == 'case-for-seastar':
        continue   # chapter 1 shows no code: the names count from chapter 2
    text = (book / 'chapters' / f'{chapter}.md').read_text()
    blocks = re.split(r'\n\s*\n', text)
    in_code = False
    for block in blocks:
        fence = block.lstrip().startswith('```')
        if fence:
            # A whole file that the book shows names its headers in #include lines.
            m = re.match(r'\s*```\{[^}]*include="([^"]+)"([^}]*)\}', block)
            if m and 'snippet=' not in m.group(2):
                source = (book / m.group(1)).read_text(errors='replace')
                for h in re.findall(r'#include <(seastar/[\w/.-]+\.hh)>', source):
                    whole_file_includes.setdefault(chapter, set()).add(h)
            continue
        for m in span.finditer(block):
            if m.group(3) is not None:
                full, is_main = resolve(m.group(3), False), False
            else:
                attrs = m.group(2)
                im = re.search(r'index="([^"]*)"', attrs)
                is_main = '.main' in attrs
                if im:
                    entry = im.group(1)
                    cm = re.match(r'`([^`]+)`', entry)
                    full = resolve(cm.group(1), True) if cm else None
                else:
                    cm = re.match(r'`([^`]+)`$', m.group(1))
                    full = resolve(cm.group(1), False) if cm else None
            if not full:
                continue
            line = text[:text.index(block)].count('\n') + 1 + block[:m.start()].count('\n')
            first.setdefault(full, (chapter, block, line))
            if is_main:
                main.setdefault(full, (chapter, block, line))

# Where each name is declared, from Universal Ctags on Seastar's headers.
seastar = book.parent.parent
tags = subprocess.run(['ctags', '-R', '--languages=C++', '--kinds-C++=+p', '--fields=+sK',
                       '--extras=+q', '-x', '--_xformat=%N %K %F %s', 'include/seastar'],
                      cwd=seastar, capture_output=True, text=True, check=True).stdout
headers = {}
for line in tags.splitlines():
    fields = line.split(' ')
    if len(fields) < 3 or fields[1] not in ('class', 'struct', 'function', 'prototype',
                                            'typedef', 'alias', 'macro', 'concept', 'enum'):
        continue
    name, scope = fields[0], fields[3] if len(fields) > 3 else ''
    full = f'{scope}::{name}' if scope else name
    headers.setdefault(full, set()).add(fields[2].removeprefix('include/'))

def transitive(h, seen=None):
    """The Seastar headers that a header <seastar/...> includes, itself
    among them, directly or not."""
    seen = set() if seen is None else seen
    h = h.strip('<>')
    if h in seen:
        return seen
    seen.add(h)
    path = seastar / 'include' / h
    if path.exists():
        for inc in re.findall(r'#include <(seastar/[\w/.-]+\.hh)>', path.read_text(errors='replace')):
            transitive(inc, seen)
    return seen

# Ctags misses some function templates, such as consume_units(), whose
# return type is on the line before the name: look for those by hand.
def find_header(full):
    name = full.rsplit('::', 1)[-1]
    found = set()
    for path in (seastar / 'include' / 'seastar').rglob('*.hh'):
        if re.search(rf'^{re.escape(name)}\(', path.read_text(errors='replace'), re.M):
            found.add(str(path.relative_to(seastar / 'include')))
    return found

# Names that the book only mentions in passing, before the chapter that
# explains them is written. Remove each from here when its chapter is,
# and give it a .main entry there.
LATER = {
    'seastar::sharded', 'seastar::foreign_ptr',    # Part III
    'seastar::file',                                # Part IV
    'seastar::thread',                              # Seastar threads
    'seastar::http::client',                        # HTTP
    'seastar::memory::stats',                       # finding memory leaks
}

# Which headers the book has named so far, chapter by chapter.
named_before = {}
named = set()
for chapter in all_chapters:
    text = (book / 'chapters' / f'{chapter}.md').read_text()
    named |= set(h[1:-1] for h in header.findall(text))
    named |= whole_file_includes.get(chapter, set())
    named_before[chapter] = set(named)

missing = {}
for full, place in first.items():
    chapter, block, line = main.get(full, place)
    if header.search(block):
        continue
    if full in LATER:
        continue
    known = headers.get(full) or find_header(full)
    if known & named_before[chapter]:
        continue   # its header is named already, for another name
    if full not in main and known & named_before[all_chapters[-1]]:
        continue   # only mentioned in passing, and its header is named somewhere
    if any(transitive(h) & known for h in header.findall(block)):
        continue   # the paragraph names a header that includes it
    missing.setdefault(chapter, []).append((line, full, full in main, sorted(known)))

for chapter in chapters:
    for line, full, is_main, known in missing.get(chapter, []):
        print(f'{chapter}.md:{line}: {full.removeprefix("seastar::")}'
              + ('' if is_main else '  (first mention)')
              + f'  [{", ".join(known) or "header unknown"}]')
