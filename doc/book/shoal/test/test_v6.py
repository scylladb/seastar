# Tests for Shoal version 6: lists (LPUSH, RPUSH, LPOP, RPOP, LLEN, LRANGE),
# BLPOP, the limit on the number of clients, and the limit on the memory of
# the requests being read. Like all of Shoal's tests, they pass against
# Valkey too.
import socket
import threading
import time
import uuid

import pytest
import redis


def unique_key():
    return 'key-' + uuid.uuid4().hex


def command(*args):
    out = b'*%d\r\n' % len(args)
    for a in args:
        a = a if isinstance(a, bytes) else a.encode()
        out += b'$%d\r\n%s\r\n' % (len(a), a)
    return out


def read_exactly(s, n):
    data = b''
    while len(data) < n:
        chunk = s.recv(n - len(data))
        assert chunk, 'connection closed'
        data += chunk
    return data


def read_reply_line(s):
    data = b''
    while not data.endswith(b'\r\n'):
        chunk = s.recv(1)
        if not chunk:
            return data
        data += chunk
    return data


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


# Check that RPUSH and LPUSH add elements at the two ends of a list, and
# return its length, and that LRANGE and LLEN see them.
def test_push_and_range(r):
    k = unique_key()
    assert r.rpush(k, 'a', 'b', 'c') == 3
    assert r.lpush(k, 'x', 'y') == 5
    assert r.lrange(k, 0, -1) == [b'y', b'x', b'a', b'b', b'c']
    assert r.llen(k) == 5


# Check LRANGE's indexes: negative ones count from the end, and ranges
# beyond the list are cut to it.
def test_lrange_indexes(r):
    k = unique_key()
    r.rpush(k, 'a', 'b', 'c', 'd')
    assert r.lrange(k, 1, 2) == [b'b', b'c']
    assert r.lrange(k, -2, -1) == [b'c', b'd']
    assert r.lrange(k, 0, 100) == [b'a', b'b', b'c', b'd']
    assert r.lrange(k, -100, 1) == [b'a', b'b']
    assert r.lrange(k, 3, 1) == []
    assert r.lrange(k, 10, 20) == []
    assert r.lrange(unique_key(), 0, -1) == []


# Check that LPOP and RPOP remove elements from the two ends, that a list
# that becomes empty is deleted, and that popping a missing key returns nil.
def test_pop(r):
    k = unique_key()
    r.rpush(k, 'a', 'b', 'c')
    assert r.lpop(k) == b'a'
    assert r.rpop(k) == b'c'
    assert r.lpop(k) == b'b'
    assert r.exists(k) == 0
    assert r.lpop(k) is None
    assert r.rpop(k) is None
    assert r.llen(k) == 0


# Check that list commands refuse a key that holds a string, and string
# commands a key that holds a list, but that SET and DEL work on any key.
def test_wrong_type(r):
    s = unique_key()
    r.set(s, 'v')
    for f in (lambda: r.lpush(s, 'x'), lambda: r.rpush(s, 'x'),
              lambda: r.lpop(s), lambda: r.llen(s), lambda: r.lrange(s, 0, -1)):
        with pytest.raises(redis.ResponseError, match='WRONGTYPE'):
            f()
    k = unique_key()
    r.rpush(k, 'a')
    with pytest.raises(redis.ResponseError, match='WRONGTYPE'):
        r.get(k)
    r.set(k, 'v')
    assert r.get(k) == b'v'
    r.rpush(s + 'x', 'a')
    assert r.delete(s + 'x') == 1


# Check that a list can have an expiration time, like any key.
def test_list_expires(r):
    k = unique_key()
    r.rpush(k, 'a')
    assert r.expire(k, 100) is True
    assert r.ttl(k) == 100
    assert r.expire(k, 0) is True
    assert r.exists(k) == 0


# Check that BLPOP returns at once, with the key and the element, if a list
# has an element, and takes it from the first non-empty key it was given.
def test_blpop_immediate(r):
    k1, k2 = unique_key(), unique_key()
    r.rpush(k2, 'a', 'b')
    assert r.blpop([k1, k2], 1) == (k2.encode(), b'a')
    assert r.lrange(k2, 0, -1) == [b'b']


# Check that BLPOP waits until its timeout, and then returns nil.
def test_blpop_timeout(r):
    k = unique_key()
    start = time.time()
    assert r.blpop([k], 0.2) is None
    assert time.time() - start >= 0.19


# Check that a client blocked in BLPOP gets an element that another client
# pushes, and that the element is removed from the list.
def test_blpop_wakes_up(r, server):
    k = unique_key()
    t, result = blpop_in_thread(server, [k], 5)
    wait_blocked(r, 1)
    assert r.rpush(k, 'v') == 1
    t.join()
    assert result == [(k.encode(), b'v')]
    assert r.exists(k) == 0


# Check that a client blocked on several keys gets the element of the one
# that is pushed to, and is then no longer waiting for the others.
def test_blpop_several_keys(r, server):
    k1, k2 = unique_key(), unique_key()
    t, result = blpop_in_thread(server, [k1, k2], 5)
    wait_blocked(r, 1)
    r.rpush(k2, 'v')
    t.join()
    assert result == [(k2.encode(), b'v')]
    r.rpush(k1, 'w')
    time.sleep(0.1)
    assert r.lrange(k1, 0, -1) == [b'w']


# Check that clients blocked on the same key are served in the order in
# which they blocked.
def test_blpop_order(r, server):
    k = unique_key()
    t1, result1 = blpop_in_thread(server, [k], 5)
    wait_blocked(r, 1)
    t2, result2 = blpop_in_thread(server, [k], 5)
    wait_blocked(r, 2)
    r.rpush(k, 'first', 'second')
    t1.join()
    t2.join()
    assert result1 == [(k.encode(), b'first')]
    assert result2 == [(k.encode(), b'second')]


# Check that LPUSH of several elements gives a blocked client the one at
# the head of the list after the push, and keeps the rest.
def test_blpop_lpush_several(r, server):
    k = unique_key()
    t, result = blpop_in_thread(server, [k], 5)
    wait_blocked(r, 1)
    assert r.lpush(k, 'a', 'b', 'c') == 3
    t.join()
    assert result == [(k.encode(), b'c')]
    assert r.lrange(k, 0, -1) == [b'b', b'a']


# Check that a pushed element goes to the blocked client even if another
# client tries to pop it right after the push, in the same pipeline.
def test_blpop_gets_element_first(r, server):
    k = unique_key()
    t, result = blpop_in_thread(server, [k], 5)
    wait_blocked(r, 1)
    with socket.create_connection(server, timeout=5) as s:
        s.sendall(command('RPUSH', k, 'v') + command('LPOP', k))
        assert read_exactly(s, len(b':1\r\n$-1\r\n')) == b':1\r\n$-1\r\n'
    t.join()
    assert result == [(k.encode(), b'v')]


# Check that a client that disconnects while blocked in BLPOP stops
# waiting, so that an element pushed later stays in the list.
def test_blpop_disconnect(r, server):
    k = unique_key()
    s = socket.create_connection(server, timeout=5)
    s.sendall(command('BLPOP', k, '0'))
    wait_blocked(r, 1)
    s.close()
    time.sleep(0.2)
    r.rpush(k, 'v')
    time.sleep(0.1)
    assert r.lrange(k, 0, -1) == [b'v']


# Check BLPOP's errors: a key holding a string, and a bad timeout.
def test_blpop_errors(r):
    s = unique_key()
    r.set(s, 'v')
    with pytest.raises(redis.ResponseError, match='WRONGTYPE'):
        r.blpop([s], 1)
    k = unique_key()
    with pytest.raises(redis.ResponseError, match='timeout is negative'):
        r.execute_command('BLPOP', k, '-1')
    with pytest.raises(redis.ResponseError, match='timeout is not a float or out of range'):
        r.execute_command('BLPOP', k, 'abc')


# Check that the server refuses connections beyond its limit on clients
# (the test runner starts it with --maxclients 64), with an error, and
# accepts new ones once others have closed.
def test_max_clients(server):
    sockets = []
    try:
        replies = []
        for _ in range(70):
            s = socket.create_connection(server, timeout=5)
            sockets.append(s)
            s.sendall(command('PING'))
            replies.append(read_reply_line(s))
        assert replies[0] == b'+PONG\r\n'
        assert replies[-1] == b'-ERR max number of clients reached\r\n'
        refused = replies.index(b'-ERR max number of clients reached\r\n')
        assert all(x == b'+PONG\r\n' for x in replies[:refused])
        assert all(x == b'-ERR max number of clients reached\r\n' for x in replies[refused:])
    finally:
        for s in sockets:
            s.close()
    time.sleep(0.2)
    with socket.create_connection(server, timeout=5) as s:
        s.sendall(command('PING'))
        assert read_reply_line(s) == b'+PONG\r\n'


# Check that many clients sending large values at once all succeed: the
# server mustn't try to hold all the values in memory at the same time
# (the test runner gives Shoal 512 MB per shard, less than all of them).
def test_many_large_values(server):
    size = 60 << 20
    clients = 10
    replies = [None] * clients

    def send(i):
        with socket.create_connection(server, timeout=30) as s:
            s.sendall(b'*3\r\n$3\r\nSET\r\n$5\r\nlarge\r\n$%d\r\n' % size)
            chunk = b'x' * (1 << 20)
            for _ in range(size // len(chunk)):
                s.sendall(chunk)
            s.sendall(b'\r\n')
            replies[i] = read_reply_line(s)
    threads = [threading.Thread(target=send, args=(i,)) for i in range(clients)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    assert replies == [b'+OK\r\n'] * clients
