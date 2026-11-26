#!/usr/bin/python3
# Chapter 12, exercise 4: starts Shoal with --shutdown-grace 100, makes a
# client that never reads its replies, and tells how long Shoal takes to
# stop after one SIGINT, and after a second one, five seconds later.
# Usage: clean-shutdown-ex4.py SHOAL
import signal
import socket
import subprocess
import sys
import time

import redis

port = 6598
shoal = subprocess.Popen([sys.argv[1], '-c1', '-m1G', '--port', str(port),
                          '--shutdown-grace', '100'],
                         stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL)
time.sleep(1)
r = redis.Redis(port=port)
r.set('big', 'x' * 1000000)
lazy = socket.create_connection(('localhost', port))
lazy.sendall(b'*2\r\n$3\r\nGET\r\n$3\r\nbig\r\n' * 100)
r.ping()


def wait_for_exit(seconds):
    start = time.monotonic()
    try:
        status = shoal.wait(timeout=seconds)
        return f'exited with status {status} after ' \
               f'{time.monotonic() - start:.2f} s'
    except subprocess.TimeoutExpired:
        return f'still running after {seconds} s'


shoal.send_signal(signal.SIGINT)
print('the first SIGINT:', wait_for_exit(5))
if shoal.poll() is None:
    shoal.send_signal(signal.SIGINT)
    print('the second SIGINT:', wait_for_exit(10))
if shoal.poll() is None:
    shoal.kill()
    shoal.wait()
