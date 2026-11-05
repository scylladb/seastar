#!/usr/bin/python3
# Chapter 15, exercise 2: the time of PUBLISH to a channel with one
# subscriber: 100,000 PUBLISHes, in pipelines of 1,000, from a client
# connected after the subscriber (so on another shard).
# Usage: designing-a-sharded-application-ex2.py PORT
import socket
import sys
import threading
import time

port = int(sys.argv[1])


def command(*args):
    out = b'*%d\r\n' % len(args)
    for a in args:
        out += b'$%d\r\n%s\r\n' % (len(a), a)
    return out


subscriber = socket.create_connection(('localhost', port))
subscriber.sendall(command(b'SUBSCRIBE', b'ch'))
subscriber.recv(100)


def drain():
    while subscriber.recv(1 << 16):
        pass


threading.Thread(target=drain, daemon=True).start()
publisher = socket.create_connection(('localhost', port))
batch = command(b'PUBLISH', b'ch', b'hello') * 1000
start = time.time()
for _ in range(100):
    publisher.sendall(batch)
    replies = b''
    while replies.count(b'\r\n') < 1000:
        replies += publisher.recv(1 << 16)
    assert replies == b':1\r\n' * 1000, replies[:20]
secs = time.time() - start
print(f'{secs * 1e6 / 100_000:.2f} us per PUBLISH')
