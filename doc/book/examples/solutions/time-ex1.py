#!/usr/bin/python3
# Chapter 9, exercise 1: what PTTL returns right after PEXPIRE key 1000,
# in 1,000 tries, sent one after the other, or both in one pipeline.
# Usage: time-ex1.py PORT
import collections
import sys

import redis

r = redis.Redis(port=int(sys.argv[1]))
r.set('k', 'v')
for how in ('one after the other', 'in one pipeline'):
    seen = collections.Counter()
    for _ in range(1000):
        if how == 'in one pipeline':
            pipe = r.pipeline(transaction=False)
            pipe.pexpire('k', 1000)
            pipe.pttl('k')
            seen[pipe.execute()[1]] += 1
        else:
            r.pexpire('k', 1000)
            seen[r.pttl('k')] += 1
    print(f'{how}:', ', '.join(f'{ttl} ({n} times)'
                               for ttl, n in sorted(seen.items())))
