# Tests for Shoal version 8, which divides the keys among its shards, and
# serves clients on all of them: commands on many keys (MGET, MSET, DBSIZE,
# and those of earlier versions), pipelines, and clients connected at the
# same time, which Shoal spreads over its shards. Like all of Shoal's
# tests, they pass against Valkey too.
import socket
import threading
import time
import uuid

import pytest
import redis


def unique_key():
    return 'key-' + uuid.uuid4().hex


# Check that MGET returns the values of keys, in their order, with nil for
# a key that doesn't exist or holds a list.
def test_mget(r):
    keys = [unique_key() for _ in range(20)]
    for i, k in enumerate(keys[:10]):
        r.set(k, f'v{i}')
    r.rpush(keys[10], 'a')
    values = r.mget(keys)
    assert values[:10] == [f'v{i}'.encode() for i in range(10)]
    assert values[10:] == [None] * 10


# Check that MSET sets many keys, and removes their expiration times, as
# SET does, and that it needs an even number of arguments after its name.
def test_mset(r):
    keys = [unique_key() for _ in range(20)]
    r.set(keys[0], 'old', ex=100)
    assert r.mset({k: f'v{i}' for i, k in enumerate(keys)}) is True
    assert r.mget(keys) == [f'v{i}'.encode() for i in range(20)]
    assert r.ttl(keys[0]) == -1
    with pytest.raises(redis.ResponseError, match='wrong number of arguments'):
        r.execute_command('MSET', keys[0], 'a', keys[1])


# Check that DEL and EXISTS count keys on all the shards, and count a key
# given twice twice.
def test_del_exists_many_keys(r):
    keys = [unique_key() for _ in range(50)]
    r.mset({k: 'x' for k in keys})
    assert r.exists(*keys, keys[0]) == 51
    assert r.delete(*keys[:25], unique_key()) == 25
    assert r.exists(*keys) == 25


# Check that KEYS finds the matching keys of all the shards, and that
# DBSIZE counts them.
def test_keys_dbsize(r):
    prefix = unique_key()
    keys = {f'{prefix}-{i}' for i in range(100)}
    before = r.dbsize()
    r.mset({k: 'x' for k in keys})
    assert {k.decode() for k in r.keys(f'{prefix}-*')} == keys
    assert r.dbsize() == before + 100
    r.delete(*keys)


# Check that the replies to a long pipeline come in the order of its
# requests, and that each request sees the effects of the earlier ones.
def test_pipeline_order(r):
    keys = [unique_key() for _ in range(20)]
    pipe = r.pipeline(transaction=False)
    for i in range(1000):
        k = keys[i % 20]
        pipe.set(k, str(i))
        pipe.get(k)
        pipe.rpush(k + '-list', str(i))
    replies = pipe.execute()
    for i in range(1000):
        assert replies[3 * i] is True
        assert replies[3 * i + 1] == str(i).encode()
        assert replies[3 * i + 2] == i // 20 + 1
    r.delete(*keys, *(k + '-list' for k in keys))


# Check that many clients connected at once, which the server may serve on
# different shards, each see their own keys and each other's.
def test_many_clients(server):
    host, port = server
    clients = [redis.Redis(host=host, port=port) for _ in range(8)]
    keys = [unique_key() for _ in range(8)]
    try:
        for c, k in zip(clients, keys):
            c.set(k, k)
        for c in clients:
            assert c.mget(keys) == [k.encode() for k in keys]
        assert clients[0].delete(*keys) == 8
    finally:
        for c in clients:
            c.close()


# Check that PUBLISH reaches subscribers connected at the same time, which
# the server may serve on different shards, and counts them all.
def test_publish_to_many_subscribers(r, server):
    host, port = server
    channel = unique_key()
    subscribers = []
    try:
        for _ in range(6):
            p = redis.Redis(host=host, port=port).pubsub()
            p.subscribe(channel)
            assert p.get_message(timeout=5)['type'] == 'subscribe'
            subscribers.append(p)
        assert r.publish(channel, 'hello') == 6
        for p in subscribers:
            m = p.get_message(timeout=5)
            assert m['type'] == 'message' and m['data'] == b'hello'
    finally:
        for p in subscribers:
            p.close()


# Starts BLPOP on a connection of its own, in a thread, and returns the
# thread and a list that receives the reply.
def blpop_in_thread(server, keys, timeout):
    host, port = server
    result = []
    client = redis.Redis(host=host, port=port)

    def run():
        try:
            result.append(client.blpop(keys, timeout))
        finally:
            client.close()
    t = threading.Thread(target=run)
    t.start()
    return t, result


# Waits until the server has the given number of clients blocked in BLPOP
# (or a short while, for a server that doesn't report it).
def wait_blocked(r, n):
    deadline = time.time() + 2
    while time.time() < deadline:
        try:
            if r.info('clients')['blocked_clients'] >= n:
                return
        except redis.ResponseError:
            break
        time.sleep(0.01)
    time.sleep(0.2)


# Check that BLPOP on many keys, which belong to different shards, gets an
# element pushed to any of them, once, and leaves the rest.
def test_blpop_many_keys(r, server):
    keys = [unique_key() for _ in range(10)]
    t, result = blpop_in_thread(server, keys, 5)
    wait_blocked(r, 1)
    r.rpush(keys[7], 'a', 'b')
    r.rpush(keys[3], 'c')
    t.join()
    assert result == [(keys[7].encode(), b'a')]
    assert r.lrange(keys[7], 0, -1) == [b'b']
    assert r.lrange(keys[3], 0, -1) == [b'c']
    r.delete(*keys)


# Check that clients blocked on overlapping sets of keys each get one
# element, and that no element is lost or given twice.
def test_blpop_many_clients(r, server):
    keys = [unique_key() for _ in range(4)]
    waiting = [blpop_in_thread(server, keys[i:i + 2], 5) for i in range(3)]
    wait_blocked(r, 3)
    for i, k in enumerate(keys):
        r.rpush(k, f'e{i}')
    got = []
    for t, result in waiting:
        t.join()
        assert len(result) == 1 and result[0] is not None
        got.append(result[0][1])
    left = [e for k in keys for e in r.lrange(k, 0, -1)]
    assert sorted(got + left) == [b'e0', b'e1', b'e2', b'e3']
    r.delete(*keys)
