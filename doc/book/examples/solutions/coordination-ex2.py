#!/usr/bin/python3
# Chapter 10, exercise 2: three clients, each on a connection of its own,
# send at once an RPUSH of two elements of 40 MB. Prints what each one
# got within 10 seconds. Usage: coordination-ex2.py PORT
import socket
import sys
import threading

port = int(sys.argv[1])
size = 40 << 20                 # 40 MB, in bytes
replies = [None] * 3


def send(i):
    chunk = b'x' * (1 << 20)
    try:
        with socket.create_connection(('localhost', port)) as s:
            s.settimeout(10)
            s.sendall(b'*4\r\n$5\r\nRPUSH\r\n$4\r\nlist\r\n')
            for _ in range(2):
                s.sendall(b'$%d\r\n' % size)
                for _ in range(size // len(chunk)):
                    s.sendall(chunk)
                s.sendall(b'\r\n')
            replies[i] = s.recv(100)
    except OSError as e:
        replies[i] = e


threads = [threading.Thread(target=send, args=(i,)) for i in range(3)]
for t in threads:
    t.start()
for t in threads:
    t.join()
for i, reply in enumerate(replies):
    print(f'client {i}: {reply!r}')
