#!/usr/bin/python3 -I
# Report passages of a chapter that repeat other documents word for word,
# so the book doesn't copy text (for example from Seastar's README or docs).
#
# Usage: check_overlap.py [-n WORDS] CHAPTER.md SOURCE...
#
# Prints every run of at least WORDS (default 6) consecutive words that
# CHAPTER.md shares with each SOURCE, ignoring case and punctuation.

import argparse
import re
from pathlib import Path

def words(path):
    return re.findall(r"[a-z0-9']+", Path(path).read_text().lower())

parser = argparse.ArgumentParser()
parser.add_argument('-n', type=int, default=6, help='minimum run length, in words')
parser.add_argument('chapter', type=Path)
parser.add_argument('sources', nargs='+', type=Path)
args = parser.parse_args()

chapter = words(args.chapter)
n = args.n
for source in args.sources:
    w = words(source)
    grams = {tuple(w[i:i + n]) for i in range(len(w) - n + 1)}
    hits = [i for i in range(len(chapter) - n + 1) if tuple(chapter[i:i + n]) in grams]
    runs = []
    for i in hits:
        if runs and i == runs[-1][1] + 1:
            runs[-1][1] = i
        else:
            runs.append([i, i])
    print(f'{source}: {len(runs)} shared runs of {n} or more words')
    for start, end in runs:
        print('   ', ' '.join(chapter[start:end + n]))
