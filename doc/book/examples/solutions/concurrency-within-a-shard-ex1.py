#!/usr/bin/python3
# Chapter 7, exercise 1: two publishers send 100 KB messages to one
# subscriber, which reads slowly and sends PINGs of its own, so that writes
# to its connection overlap if nothing orders them.
# Usage: concurrency-within-a-shard-ex1.py PORT
import socket
import sys
import threading
import time

port = int(sys.argv[1])

def command(*args):
    out = b'*%d\r\n' % len(args)
    for a in args:
        a = a if isinstance(a, bytes) else a.encode()
        out += b'$%d\r\n%s\r\n' % (len(a), a)
    return out

sub = socket.create_connection(('localhost', port))
sub.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
sub.sendall(command('SUBSCRIBE', 'ch'))
time.sleep(0.2)

def publisher():
    try:
        with socket.create_connection(('localhost', port)) as s:
            for _ in range(200):
                s.sendall(command('PUBLISH', 'ch', b'x' * 100000))
                s.recv(100)
    except OSError as e:
        print('publisher:', e)

threads = [threading.Thread(target=publisher) for _ in range(2)]
for t in threads:
    t.start()
received = 0
try:
    for _ in range(500):
        sub.sendall(command('PING'))
        data = sub.recv(4096)
        if not data:
            print("the subscriber's connection closed")
            break
        received += len(data)
        time.sleep(0.005)
except OSError as e:
    print('subscriber:', e)
for t in threads:
    t.join()
print(f'subscriber received {received} bytes')
