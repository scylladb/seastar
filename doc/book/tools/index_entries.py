#!/usr/bin/python3 -I
# Prints the PDF's index as plain text, one entry per line with its pages,
# from the makeindex output of the last build (build/gen/latex-book/book.ind),
# to review the entries for consistency: singular and plural, capitalization,
# two names for one thing.
#
# Usage: index_entries.py [BUILD_DIR]   (default: build)

import re
import sys
from pathlib import Path

build = Path(sys.argv[1] if len(sys.argv) > 1 else 'build')
ind = (build / 'gen' / 'latex-book' / 'book.ind').read_text()

def plain(s):
    s = re.sub(r'\\hyperx?indexformat\{\\textbf\}\{([^}]*)\}', r'*\1*', s)
    s = re.sub(r'\\hyperpage\{([^}]*)\}', r'\1', s)
    s = re.sub(r'\\textbf\s*\{([^}]*)\}', r'*\1*', s)
    s = re.sub(r'\\texttt\s*\{([^}]*)\}', r'\1', s)
    s = s.replace('\\allowbreak{}', '').replace('\\_', '_').replace('\\&', '&')
    s = re.sub(r'\\delimiter\s*"[0-9A-F]+\s*', '–', s)
    s = re.sub(r'\\[a-z]+\s*', '', s)
    return s.replace('{', '').replace('}', '').strip()

# makeindex wraps long lines; join each entry's continuation lines to it.
# A continuation line is indented with a tab.
ind = re.sub(r'\n\t+', ' ', ind)

for line in ind.splitlines():
    m = re.match(r'\s*\\(item|subitem|subsubitem)\s+(.*)', line)
    if m:
        depth = {'item': 0, 'subitem': 1, 'subsubitem': 2}[m.group(1)]
        print('    ' * depth + plain(m.group(2)))
    elif line.strip() and not line.strip().startswith(('\\begin', '\\end', '\\indexspace', '\\par')):
        if re.match(r'\s*\{\\bfseries', line):
            print('== ' + plain(line))
