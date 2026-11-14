#!/usr/bin/python3
# Chapter 4, exercise 6: a million APPENDs, sent in a single pipeline,
# whose replies the client reads only after sending all of them.
# Usage: coroutines-ex6.py PORT
import sys
import time

import redis

port = int(sys.argv[1])
# Give up after 20 seconds without any progress on the socket.
r = redis.Redis(port=port, socket_timeout=20)
r.delete('appended')
pipe = r.pipeline(transaction=False)
for _ in range(1_000_000):
    pipe.append('appended', 'x')
start = time.time()
try:
    pipe.execute()
    print(f'done in {time.time() - start:.2f} s')
except redis.TimeoutError:
    print(f'stuck: no progress for 20 seconds, '
          f'{time.time() - start:.0f} s after starting')
