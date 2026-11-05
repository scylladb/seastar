#!/usr/bin/python3 -I
# Lists the terms in italics that have no index marker, chapter by chapter.
# A term the text defines is set in italics, and should usually be indexed
# there, with .main (see the README's "The index"); most other italics are
# just emphasis, so read the list and index only the real terms.
#
# Usage: unindexed_terms.py [CHAPTER...]   (default: all chapters, in order)

import re
import sys
from pathlib import Path

book = Path(__file__).resolve().parent.parent
names = sys.argv[1:] or [line.strip()[2:] for line in (book / 'metadata.yaml').read_text().splitlines()
                         if re.match(r'\s+- [a-z0-9-]+$', line)]
for name in names:
    text = (book / 'chapters' / f'{name}.md').read_text()
    text = re.sub(r'```.*?```', '', text, flags=re.S)
    terms = []
    for m in re.finditer(r'(?<![*\[\w])\*([^*\n]{1,40}?)\*(?![*\]])', text):
        term = m.group(1)
        if term[0].isupper() and ' ' in term:   # a title, or an emphasized phrase
            continue
        terms.append(term)
    if terms:
        print(f'{name}: ' + '; '.join(terms))
