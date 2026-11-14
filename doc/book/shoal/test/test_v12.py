# Tests for Shoal version 12, which keeps its keys in a table of many small
# tables, and can evict keys to stay within --maxmemory, as Redis's
# allkeys-lru policy does. Like all of Shoal's tests, they pass against
# Valkey too, which they run with the same options (and, with
# --maxmemory, --maxmemory-policy allkeys-lru).
import time

import redis


def with_maxmemory(start_server, maxmemory):
    options = ['--maxmemory', str(maxmemory)]
    if start_server.is_valkey:
        options += ['--maxmemory-policy', 'allkeys-lru']
    srv = start_server(*options)
    return srv, redis.Redis(port=srv.address[1])


# Check that keys of every length work: up to 23 characters, which the table
# keeps inline, and longer.
def test_long_keys(r):
    keys = ['k' * n for n in (1, 15, 16, 23, 24, 31, 32, 100, 1000)]
    for k in keys:
        r.set(k, k.upper())
    for k in keys:
        assert r.get(k) == k.upper().encode()
    assert r.delete(*keys) == len(keys)


# Check that many keys, which make the table split its small tables again
# and again, can all be found, and that KEYS returns each of them once.
def test_many_keys(r):
    n = 200000
    p = r.pipeline(transaction=False)
    for i in range(n):
        p.set(f'many:{i}', str(i))
    p.execute()
    p = r.pipeline(transaction=False)
    for i in range(0, n, 7):
        p.get(f'many:{i}')
    assert p.execute() == [str(i).encode() for i in range(0, n, 7)]
    keys = r.keys('many:*')
    assert len(keys) == n
    assert len(set(keys)) == n
    p = r.pipeline(transaction=False)
    for i in range(n):
        p.delete(f'many:{i}')
    p.execute()


# Valkey's clock for least-recently-used eviction ticks once a second, so the
# tests wait that long between the keys they expect to be evicted and those
# they expect to keep.
lru_tick = 1.1


def set_many(r, prefix, n, value):
    p = r.pipeline(transaction=False)
    for i in range(n):
        p.set(f'{prefix}:{i}', value)
    p.execute()


# Check that with --maxmemory, writing much more than the limit evicts keys,
# keeps the memory used within about the limit, and keeps the keys written
# last.
def test_maxmemory_evicts(start_server):
    maxmemory = 10 << 20
    srv, r = with_maxmemory(start_server, maxmemory)
    value = 'x' * 1000
    n = 30000                       # about 30 MB of values
    set_many(r, 'evict', n, value)
    info = r.info()
    assert info['evicted_keys'] > 0
    assert info['used_memory'] < maxmemory * 1.1
    assert r.dbsize() < n
    time.sleep(lru_tick)
    set_many(r, 'last', 100, value)
    p = r.pipeline(transaction=False)
    for i in range(100):
        p.get(f'last:{i}')
    assert all(v == value.encode() for v in p.execute())


# Check that a key read again and again isn't evicted, though older than
# most: eviction prefers the keys used least recently.
def test_maxmemory_keeps_used_keys(start_server):
    srv, r = with_maxmemory(start_server, 10 << 20)
    value = 'x' * 1000
    r.set('hot', 'value')
    set_many(r, 'old', 7000, value)     # about 7 MB, below the limit
    time.sleep(lru_tick)
    assert r.get('hot') == b'value'
    set_many(r, 'new', 5000, value)     # over the limit: evicts old keys
    assert r.info()['evicted_keys'] > 0
    assert r.get('hot') == b'value'
