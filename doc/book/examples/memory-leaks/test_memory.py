# A test that repeats the same work against Shoal, again and again, and
# checks that the objects that each shard has allocated stop growing: a
# leak of memory that Shoal still points to, which LeakSanitizer can't
# find, makes it grow on every round. Run it with Shoal's other tests,
# against a release or dev build:
#   shoal/test/run 16 --test examples/memory-leaks/test_memory.py \
#       -k test_memory_stops_growing
import re
import socket
import time
import urllib.request

import pytest
import redis

from test_v8 import unique_key


def free_port():
    with socket.socket() as s:
        s.bind(('localhost', 0))
        return s.getsockname()[1]


# snippet: allocated
def live_objects(admin_port):
    """The number of objects allocated on each shard and not yet freed,
    from Seastar's metrics."""
    text = urllib.request.urlopen(f'http://localhost:{admin_port}/metrics').read().decode()
    return {int(shard): int(float(value)) for shard, value in re.findall(
        r'^shoal_memory_malloc_live_objects\{shard="(\d+)"\} (\S+)$', text, re.M)}
# end snippet: allocated


def resp(*words):
    """A request of these words, in RESP."""
    return b'*%d\r\n' % len(words) + b''.join(
        b'$%d\r\n%s\r\n' % (len(w), w) for w in words)


# snippet: workload
def workload(port):
    """One round of work, of many kinds, by clients that come and go."""
    for _ in range(10):
        with redis.Redis(port=port) as r:
            key = unique_key()
            r.set(key, 'x' * 1000)
            r.get(key)
            r.lpush(key + 'l', *range(100))
            r.lrange(key + 'l', 0, -1)
            r.expire(key, 1000)
            r.delete(key, key + 'l')
            r.blpop([key + 'b'], timeout=0.01)
            r.publish('channel', 'message')
            with pytest.raises(redis.ResponseError):
                r.execute_command('NOSUCHCOMMAND')
    # Clients that subscribe to a channel, or wait in BLPOP for a key, of
    # their own, and hang up without unsubscribing, or waiting for the reply.
    clients = []
    for command in (b'SUBSCRIBE', b'BLPOP'):
        for _ in range(50):
            key = unique_key().encode()
            s = socket.create_connection(('localhost', port))
            s.sendall(resp(command, key, b'0') if command == b'BLPOP'
                      else resp(command, key))
            clients.append(s)
    time.sleep(0.1)
    for s in clients:
        s.close()
# end snippet: workload


# snippet: test
# Check that after a few rounds of warm-up, more rounds of the same work
# don't leave more objects allocated on any shard than a few.
def test_memory_stops_growing(start_server):
    if start_server.is_valkey:
        pytest.skip('Valkey has no Seastar metrics')
    admin = free_port()
    srv = start_server('--admin-port', str(admin))
    port = srv.address[1]
    for _ in range(3):
        workload(port)
    time.sleep(0.5)
    before = live_objects(admin)
    for _ in range(10):
        workload(port)
    time.sleep(0.5)
    after = live_objects(admin)
    growth = {shard: after[shard] - before[shard] for shard in after}
    print(f'objects left after 10 more rounds, by shard: {growth}')
    assert all(g < 100 for g in growth.values()), growth
# end snippet: test
