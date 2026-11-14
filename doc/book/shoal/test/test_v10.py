# Tests for Shoal version 10, which keeps its data in append-only logs:
# the data survives a restart, a crash (with --appendfsync always), and a
# log whose end was cut short by a crash. Like all of Shoal's tests, they
# pass against Valkey too, which they run with the same options.
import os
import signal
import time
import uuid
from pathlib import Path

import pytest
import redis


def unique_key():
    return 'key-' + uuid.uuid4().hex


def persistent(start_server, data, fsync='everysec'):
    """Starts a server of the test's own that keeps its data in the
    directory data, and returns it with a client. (Shoal always keeps a
    log; Valkey needs to be told.)"""
    options = ['--appendfsync', fsync, '--dir', str(data)]
    if start_server.is_valkey:
        options += ['--appendonly', 'yes']
    srv = start_server(*options)
    return srv, redis.Redis(port=srv.address[1])


def stop(srv, sig=signal.SIGTERM):
    srv.process.send_signal(sig)
    srv.process.wait(timeout=10)


# Check that strings, lists and expiration times survive a clean restart.
def test_restart(start_server, tmp_path):
    srv, r = persistent(start_server, tmp_path)
    r.set('s', 'hello')
    r.set('gone', 'x')
    r.delete('gone')
    r.rpush('l', 'a', 'b', 'c')
    r.lpop('l')
    r.set('t', 'v', ex=1000)
    r.set('soon', 'v', px=300)
    stop(srv)
    time.sleep(0.5)
    srv, r = persistent(start_server, tmp_path)
    assert r.get('s') == b'hello'
    assert r.get('gone') is None
    assert r.lrange('l', 0, -1) == [b'b', b'c']
    # The expiration time was kept as a time, not as a duration: it
    # didn't start again.
    assert 990 <= r.ttl('t') <= 1000
    assert r.get('soon') is None
    stop(srv)


# Check that with --appendfsync always, every change acknowledged before a
# crash (kill -9) is still there after it.
def test_crash(start_server, tmp_path):
    srv, r = persistent(start_server, tmp_path, 'always')
    keys = [unique_key() for _ in range(100)]
    for i, k in enumerate(keys):
        r.set(k, str(i))
    stop(srv, signal.SIGKILL)
    srv, r = persistent(start_server, tmp_path, 'always')
    assert r.mget(keys) == [str(i).encode() for i in range(len(keys))]
    stop(srv)


# Check that FLUSHALL survives a restart.
def test_flushall(start_server, tmp_path):
    srv, r = persistent(start_server, tmp_path)
    r.set('a', '1')
    r.flushall()
    r.set('b', '2')
    stop(srv)
    srv, r = persistent(start_server, tmp_path)
    assert r.get('a') is None
    assert r.get('b') == b'2'
    stop(srv)


# Check that a log whose end was cut short, as a crash in the middle of a
# write leaves it, is cut after its last whole request, and that what came
# before survives, and so does what comes after, through another restart.
def test_cut_short(start_server, tmp_path):
    srv, r = persistent(start_server, tmp_path)
    r.set('before', '1')
    stop(srv)
    # Shoal's logs are shard-*.aof; Valkey's last is its *.incr.aof.
    logs = [p for p in Path(tmp_path).rglob('*.aof') if p.stat().st_size]
    newest = max(logs, key=lambda p: p.stat().st_mtime)
    with open(newest, 'ab') as f:
        f.write(b'*3\r\n$3\r\nSET\r\n$5\r\nafter\r\n$1')
    srv, r = persistent(start_server, tmp_path)
    assert r.get('before') == b'1'
    assert r.get('after') is None
    r.set('later', '2')
    stop(srv)
    srv, r = persistent(start_server, tmp_path)
    assert r.get('before') == b'1'
    assert r.get('later') == b'2'
    stop(srv)


# Check that PEXPIREAT, and SET's PXAT, which a log records, set an
# expiration time on the computer's clock, in milliseconds since 1970.
def test_pexpireat(r):
    k = unique_key()
    now = int(time.time() * 1000)
    r.set(k, 'v')
    assert r.pexpireat(k, now + 100_000)
    assert 99 <= r.ttl(k) <= 100
    assert r.set(k, 'w', pxat=now + 50_000)
    assert 49 <= r.ttl(k) <= 50
    assert r.pexpireat(k, now - 1000)
    assert r.get(k) is None
