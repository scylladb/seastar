# Tests for Shoal version 14, which replicates a primary's keys to a
# replica. Shoal's replica connects to the primary's --replication-port,
# and Valkey's to its usual port, but they behave the same.
import signal
import socket
import tempfile
import time

import pytest
import redis

from test_v9 import free_port


def start_primary(start_server, *options, port=None, replication_port=None):
    """Starts a primary, and returns it, a Redis client of it, and the
    address that a replica connects to."""
    if start_server.is_valkey:
        primary = start_server(*options, port=port)
        return primary, redis.Redis(port=primary.address[1]), primary.address[1]
    replication_port = replication_port or free_port()
    primary = start_server('--replication-port', str(replication_port),
                           *options, port=port)
    return primary, redis.Redis(port=primary.address[1]), replication_port


def start_replica(start_server, primary_port, *options):
    """Starts a replica of the primary whose replication port is
    primary_port, and returns it and a Redis client of it."""
    if start_server.is_valkey:
        replica = start_server('--replicaof', '127.0.0.1', str(primary_port),
                               *options)
    else:
        replica = start_server('--replicaof', f'127.0.0.1:{primary_port}',
                               *options)
    return replica, redis.Redis(port=replica.address[1])


def eventually(check, timeout=30):
    """Waits until check() returns true, and fails if it doesn't in time."""
    deadline = time.time() + timeout
    while not check():
        assert time.time() < deadline, 'timed out'
        time.sleep(0.05)


def wait_until_closed(port):
    """Waits until nothing accepts connections on port. A Seastar server
    killed while it uses the io_uring backend may leave its listening
    socket accepting connections for a moment after the process is gone,
    connections that are then reset."""
    deadline = time.time() + 30
    while True:
        try:
            socket.create_connection(('127.0.0.1', port), timeout=1).close()
        except ConnectionRefusedError:
            return
        except ConnectionResetError:
            pass        # the old socket, which is about to go
        assert time.time() < deadline, f'port {port} still accepts'
        time.sleep(0.05)


def same_keys(primary, replica):
    """Whether the replica has the primary's keys, with the same values."""
    keys = sorted(primary.keys())
    if sorted(replica.keys()) != keys:
        return False
    for key in keys:
        try:
            if replica.get(key) != primary.get(key):
                return False
        except redis.exceptions.ResponseError:
            # A list, which GET refuses (Shoal has no TYPE).
            if replica.lrange(key, 0, -1) != primary.lrange(key, 0, -1):
                return False
    return True


# Check that a replica copies the keys that the primary had before the
# replica started: strings, lists, and keys that expire.
def test_copy_keys(start_server):
    _, p, port = start_primary(start_server)
    p.mset({f'copy:{i}': i for i in range(10000)})
    p.rpush('copy:list', *range(3000))
    p.set('copy:expiring', 'x', ex=1000)
    _, r = start_replica(start_server, port)
    eventually(lambda: r.dbsize() == p.dbsize())
    assert same_keys(p, r)
    # The copy may take a while, in a debug build more than 10 seconds.
    assert 900 < r.ttl('copy:expiring') <= 1000


# Check that a replica applies the primary's changes, as they're made.
def test_apply_changes(start_server):
    _, p, port = start_primary(start_server)
    p.set('changes:gone', 'x')
    _, r = start_replica(start_server, port)
    eventually(lambda: r.exists('changes:gone'))
    p.mset({f'changes:{i}': i for i in range(100)})
    p.delete('changes:gone')
    p.rpush('changes:list', 'a', 'b', 'c')
    p.lpop('changes:list')
    p.expire('changes:1', 1000)
    eventually(lambda: same_keys(p, r))
    assert r.ttl('changes:1') > 990
    # Check that FLUSHALL, which each shard records, empties every shard
    # of the replica, and the replica goes on applying the next changes.
    p.flushall()
    p.set('changes:after', 'y')
    eventually(lambda: r.keys() == [b'changes:after'])


# Check that a replica's clients may read its keys, but not change them.
def test_read_only(start_server):
    _, p, port = start_primary(start_server)
    p.set('ro:a', '1')
    _, r = start_replica(start_server, port)
    eventually(lambda: r.get('ro:a') == b'1')
    for change in [lambda: r.set('ro:a', '2'), lambda: r.delete('ro:a'),
                   lambda: r.rpush('ro:list', 'x'), lambda: r.flushall()]:
        with pytest.raises(redis.exceptions.ReadOnlyError):
            change()
    assert r.get('ro:a') == b'1'
    assert r.info('replication')['role'] == 'slave'
    assert p.info('replication')['role'] == 'master'


# Check that a replica that restarts, with the keys it kept, drops those
# that the primary deleted meanwhile, and gets those it added.
def test_replica_restarts(start_server):
    _, p, port = start_primary(start_server)
    p.mset({f'restart:{i}': i for i in range(1000)})
    data = tempfile.TemporaryDirectory()
    replica, r = start_replica(start_server, port, '--dir', data.name)
    eventually(lambda: r.dbsize() == 1000)
    replica.process.terminate()
    replica.process.wait()
    p.delete('restart:7')
    p.set('restart:new', 'x')
    _, r = start_replica(start_server, port, '--dir', data.name)
    eventually(lambda: same_keys(p, r))
    data.cleanup()


# Check that a replica of a primary that restarts copies the primary's
# keys again, and goes on applying its changes.
def test_primary_restarts(start_server):
    port = free_port()
    replication_port = free_port()
    data = tempfile.TemporaryDirectory()
    primary, p, primary_port = start_primary(
        start_server, '--dir', data.name, port=port,
        replication_port=replication_port)
    p.mset({f'primary:{i}': i for i in range(1000)})
    _, r = start_replica(start_server, primary_port)
    eventually(lambda: r.dbsize() == 1000)
    primary.process.kill()
    primary.process.wait()
    wait_until_closed(port)
    wait_until_closed(replication_port)
    primary, p, _ = start_primary(
        start_server, '--dir', data.name, port=port,
        replication_port=replication_port)
    p.set('primary:new', 'x')
    eventually(lambda: same_keys(p, r))
    data.cleanup()


# Check that a replica that falls behind by more than the primary's
# backlog of changes catches up, by copying the keys again.
def test_falls_behind(start_server):
    _, p, port = start_primary(start_server, '--repl-backlog-size', '65536')
    replica, r = start_replica(start_server, port)
    p.set('behind:0', 'x')
    eventually(lambda: r.exists('behind:0'))
    # Stop the replica while the primary changes ten times its backlog.
    replica.process.send_signal(signal.SIGSTOP)
    try:
        p.mset({f'behind:{i}': 'x' * 50 for i in range(10000)})
    finally:
        replica.process.send_signal(signal.SIGCONT)
    eventually(lambda: r.dbsize() == p.dbsize())
    assert same_keys(p, r)
