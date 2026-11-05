# Tests for Shoal version 11, which rewrites its logs with BGREWRITEAOF:
# the rewritten logs keep the data, including the changes made while the
# rewrite runs; a second BGREWRITEAOF during a rewrite is refused; and
# SHUTDOWN during a rewrite stops promptly, keeping the data. Like all of
# Shoal's tests, they pass against Valkey too, except where a test says so.
import time
from pathlib import Path

import pytest
import redis

from test_v10 import persistent, stop


def wait_for_rewrite(r):
    """Waits until no rewrite is in progress, and checks that the last one
    succeeded."""
    deadline = time.time() + 60
    while r.info('persistence')['aof_rewrite_in_progress']:
        assert time.time() < deadline, 'the rewrite did not end'
        time.sleep(0.01)
    assert r.info('persistence')['aof_last_bgrewrite_status'] == 'ok'


# Check that the data survives a rewrite, and a restart after it: strings,
# lists (longer than one of the rewrite's RPUSHes), expiration times, and
# keys that were deleted.
def test_rewrite_keeps_data(start_server, tmp_path):
    srv, r = persistent(start_server, tmp_path)
    for i in range(100):
        r.set('s', f'v{i}')
    r.set('gone', 'x')
    r.delete('gone')
    r.rpush('l', *[str(i) for i in range(2500)])
    r.lpop('l')
    r.set('t', 'v', ex=1000)
    assert r.bgrewriteaof()
    wait_for_rewrite(r)
    stop(srv)
    srv, r = persistent(start_server, tmp_path)
    assert r.get('s') == b'v99'
    assert r.get('gone') is None
    assert r.lrange('l', 0, -1) == [str(i).encode() for i in range(1, 2500)]
    assert 990 <= r.ttl('t') <= 1000
    stop(srv)


# Check that changes made while a rewrite runs survive it, and a restart:
# to keys that the rewrite has written already, and to keys it hasn't yet.
def test_changes_during_rewrite(start_server, tmp_path):
    srv, r = persistent(start_server, tmp_path)
    n = 20000
    p = r.pipeline(transaction=False)
    for i in range(n):
        p.rpush(f'list{i}', 'a')
        p.set(f'str{i}', 'old')
    p.execute()
    assert r.bgrewriteaof()
    p = r.pipeline(transaction=False)
    for i in range(0, n, 7):
        p.rpush(f'list{i}', 'b')
        p.set(f'str{i}', 'new')
        p.delete(f'list{i + 1}')
        p.set(f'added{i}', 'x')
    p.execute()
    wait_for_rewrite(r)
    stop(srv)
    srv, r = persistent(start_server, tmp_path)
    p = r.pipeline(transaction=False)
    for i in range(n):
        p.lrange(f'list{i}', 0, -1)
        p.get(f'str{i}')
        p.get(f'added{i}')
    results = p.execute()
    for i in range(n):
        lst, s, added = results[3 * i:3 * i + 3]
        changed = i % 7 == 0
        deleted = i % 7 == 1
        assert lst == ([] if deleted else [b'a', b'b'] if changed else [b'a'])
        assert s == (b'new' if changed else b'old')
        assert added == (b'x' if changed else None)
    stop(srv)


# Check that a BGREWRITEAOF sent while a rewrite runs is refused.
def test_bgrewriteaof_twice(start_server, tmp_path):
    srv, r = persistent(start_server, tmp_path)
    r.set('k', 'v')
    p = r.pipeline(transaction=False)
    p.bgrewriteaof()
    p.bgrewriteaof()
    first, second = p.execute(raise_on_error=False)
    assert first is True
    assert isinstance(second, redis.ResponseError)
    assert 'already in progress' in str(second)
    wait_for_rewrite(r)
    stop(srv)


# Check that SHUTDOWN during a rewrite stops the server promptly, and that
# the data is still there when it starts again.
def test_shutdown_during_rewrite(start_server, tmp_path):
    srv, r = persistent(start_server, tmp_path)
    n = 100000
    p = r.pipeline(transaction=False)
    for i in range(n):
        p.set(f'key{i}', f'value{i}')
    p.execute()
    assert r.bgrewriteaof()
    start = time.time()
    r.shutdown()
    srv.process.wait(timeout=10)
    assert time.time() - start < 5
    srv, r = persistent(start_server, tmp_path)
    # Valkey accepts connections while it still loads its data (Shoal
    # replays its logs before it listens): wait until it has loaded.
    deadline = time.time() + 30
    while True:
        try:
            r.ping()
            break
        except redis.exceptions.BusyLoadingError:
            assert time.time() < deadline
            time.sleep(0.05)
    assert r.dbsize() == n
    assert r.get(f'key{n - 1}') == f'value{n - 1}'.encode()
    stop(srv)


# Check that the rewrite makes the logs shorter: here, a key changed a
# thousand times leaves one SET. (Valkey keeps its log in a directory of
# files of its own, appendonlydir/, which this test doesn't read.)
def test_rewrite_shortens_logs(start_server, tmp_path):
    if start_server.is_valkey:
        pytest.skip('Valkey keeps its log in files of another layout')
    # With --appendfsync always, every SET is on the disk at once.
    srv, r = persistent(start_server, tmp_path, 'always')
    for i in range(1000):
        r.set('counter', str(i))

    def logs_size():
        return sum(f.stat().st_size for f in Path(tmp_path).glob('*.aof'))
    before = logs_size()
    assert r.bgrewriteaof()
    wait_for_rewrite(r)
    assert logs_size() < before / 10
    assert r.get('counter') == b'999'
    stop(srv)
