#!/usr/bin/python3 -I
# Lists every reference from one chapter to another, such as
# "[chapter](#ch:stalls)", with the sentence around it, grouped by the
# chapter, appendix or part referred to. A reference often promises
# something ("chapter 21 shows how to ..."), so before a chapter is done,
# read the references to it, and check that it keeps those promises.
#
# Usage: cross_references.py [LABEL...]
#        cross_references.py --unlabeled
#
# With labels (such as ch:stalls), lists only the references to them.
# With --unlabeled, lists instead the sentences that seem to promise
# something of another chapter without naming it ("later chapters explain
# ..."), which the check above can't see. Better to name the chapter.

import re
import sys
from collections import defaultdict
from pathlib import Path

book = Path(__file__).resolve().parent.parent
link = re.compile(r'\[[^\]]*\]\(#((?:ch|app|part):[\w-]+)\)')

def sentences(text):
    """Splits Markdown text into sentences, roughly, ignoring code blocks."""
    text = re.sub(r'```.*?```', ' ', text, flags=re.S)
    text = re.sub(r'\s+', ' ', text)
    return re.split(r'(?<=[.!?])\s+(?=[A-Z\[`(*])', text)

vague = re.compile(r"later chapters?|a later chapter|later in the book|"
                   r"comes? back later|the rest of the book|in a chapter of its own",
                   re.IGNORECASE)

if sys.argv[1:] == ['--unlabeled']:
    for chapter in sorted((book / 'chapters').glob('*.md')):
        for sentence in sentences(chapter.read_text()):
            if vague.search(sentence) and not link.search(sentence):
                print(f'[{chapter.stem}] {sentence.strip()}')
    sys.exit(0)

refs = defaultdict(list)
for chapter in sorted((book / 'chapters').glob('*.md')):
    for sentence in sentences(chapter.read_text()):
        for target in link.findall(sentence):
            if not target.endswith(':' + chapter.stem):
                refs[target].append((chapter.stem, sentence.strip()))

wanted = sys.argv[1:]
for target in sorted(refs):
    if wanted and target not in wanted:
        continue
    print(f'== {target} ({len(refs[target])} references)')
    for source, sentence in refs[target]:
        print(f'  [{source}] {sentence}')
    print()
