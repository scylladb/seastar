#!/usr/bin/python3
# Kills a server with SIGKILL while a client sends it SETs as fast as it
# can, and then counts the acknowledged SETs that are missing after a
# restart. The server is the command given, Shoal or valkey-server, with
# --appendfsync everysec.
#   files-and-storage-ex5.py 'shoal/v10/build/shoal -c1 -m1G'
#   files-and-storage-ex5.py valkey-server
import shlex
import socket
import subprocess
import sys
import tempfile
import threading
import time

import redis

PORT = 6610
BATCH = 1000            # SETs sent together, before reading their replies


def start(command, data):
    args = [*shlex.split(command), '--port', str(PORT), '--dir', data,
            '--appendfsync', 'everysec']
    if 'valkey-server' in command:
        args += ['--appendonly', 'yes', '--save', '']
    process = subprocess.Popen(args, stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL)
    # Wait until it answers: Valkey accepts connections while it's still
    # loading its file, and answers only with errors until it's done.
    for _ in range(600):
        try:
            redis.Redis(port=PORT).ping()
            return process
        except redis.RedisError:
            time.sleep(0.1)
    raise RuntimeError('the server did not start')


def set_until_killed(acked):
    """SETs k0, k1, ... in order, and counts in acked[0] those
    acknowledged, until the connection fails."""
    s = socket.create_connection(('localhost', PORT))
    i = 0
    try:
        while True:
            batch = b''.join(
                b'*3\r\n$3\r\nSET\r\n$%d\r\nk%d\r\n$%d\r\n%d\r\n'
                % (len(str(j)) + 1, j, len(str(j)), j)
                for j in range(i, i + BATCH))
            s.sendall(batch)
            replies = b''
            while len(replies) < 5 * BATCH:
                more = s.recv(1 << 16)
                if not more:
                    return
                replies += more
                acked[0] = i + len(replies) // 5    # each is +OK\r\n
            i += BATCH
    except OSError:
        pass


def main():
    command = sys.argv[1]
    with tempfile.TemporaryDirectory() as data:
        server = start(command, data)
        acked = [0]
        client = threading.Thread(target=set_until_killed, args=(acked,))
        began = time.time()
        client.start()
        time.sleep(3.5)
        server.kill()
        rate = acked[0] / (time.time() - began)
        server.wait()
        client.join()
        server = start(command, data)
        r = redis.Redis(port=PORT)
        present = 0
        for i in range(0, acked[0], 10000):
            keys = [f'k{j}' for j in range(i, min(i + 10000, acked[0]))]
            present += sum(v is not None for v in r.mget(keys))
        missing = acked[0] - present
        print(f'{acked[0]} SETs acknowledged ({rate:.0f} a second); '
              f'{missing} missing after the restart, '
              f'{missing / rate * 1000:.1f} ms worth')
        server.kill()
        server.wait()


main()
