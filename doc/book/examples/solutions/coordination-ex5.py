#!/usr/bin/python3
# Chapter 10, exercise 5: three subscribers that never read, and a
# publisher of 1,000 messages of 100 KB. Prints how many PUBLISHes were
# done, in how long, and the last reply, waiting at most 15 seconds for
# each. Usage: coordination-ex5.py PORT
import socket
import sys
import time

port = int(sys.argv[1])
subscribers = []
for _ in range(3):
    s = socket.create_connection(('localhost', port))
    # A small receive buffer, so that little of what Shoal sends fits.
    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
    s.sendall(b'*2\r\n$9\r\nSUBSCRIBE\r\n$2\r\nch\r\n')
    subscribers.append(s)
time.sleep(0.3)
publisher = socket.create_connection(('localhost', port), timeout=15)
message = b'x' * 100000
command = b'*3\r\n$7\r\nPUBLISH\r\n$2\r\nch\r\n$%d\r\n%s\r\n' % (
    len(message), message)
done = 0
reply = b''
start = time.time()
try:
    for _ in range(1000):
        publisher.sendall(command)
        reply = b''
        while not reply.endswith(b'\r\n'):
            reply += publisher.recv(100)
        done += 1
except OSError as e:
    print(f'stopped: {e!r}')
print(f'{done} of 1000 PUBLISHes done in {time.time() - start:.1f} s,',
      f'the last reply {reply!r}')
