#!/usr/bin/python3
# Chapter 4, exercise 2: appends one byte to the same key, N times, in
# pipelines of a thousand APPENDs, and says how long it took.
# Usage: coroutines-ex2.py PORT N
import sys
import time

import redis

port, n = int(sys.argv[1]), int(sys.argv[2])
r = redis.Redis(port=port)
r.delete('appended')
start = time.time()
for done in range(0, n, 1000):
    pipe = r.pipeline(transaction=False)
    for _ in range(min(1000, n - done)):
        pipe.append('appended', 'x')
    pipe.execute()
print(f'{n} appends: {time.time() - start:.2f} s, '
      f'and the value has {len(r.get("appended"))} bytes')
