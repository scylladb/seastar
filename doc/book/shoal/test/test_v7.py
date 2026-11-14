# Tests for Shoal version 7: stopping cleanly, on SHUTDOWN, SIGINT or
# SIGTERM. Like all of Shoal's tests, they pass against Valkey too.
import signal
import socket

import pytest
import redis


def command(*args):
    out = b'*%d\r\n' % len(args)
    for a in args:
        out += b'$%d\r\n%s\r\n' % (len(a), a.encode())
    return out


@pytest.mark.parametrize('how', ['SHUTDOWN', 'SIGINT', 'SIGTERM'])
def test_stop(own_server, how):
    address = own_server.address
    idle = socket.create_connection(address, timeout=10)
    waiting = socket.create_connection(address, timeout=10)
    waiting.sendall(command('BLPOP', 'test_stop_list', '0'))
    subscriber = socket.create_connection(address, timeout=10)
    subscriber.sendall(command('SUBSCRIBE', 'test_stop_channel'))
    confirmation = b'*3\r\n$9\r\nsubscribe\r\n$17\r\ntest_stop_channel\r\n:1\r\n'
    assert subscriber.recv(len(confirmation), socket.MSG_WAITALL) == confirmation
    r = redis.Redis(*address)
    r.ping()
    if how == 'SHUTDOWN':
        with pytest.raises(redis.exceptions.ConnectionError):
            r.execute_command('SHUTDOWN')
    else:
        own_server.process.send_signal(getattr(signal, how))
    # Check that the server exits, successfully, and closes every connection,
    # without a reply to the BLPOP.
    assert own_server.process.wait(timeout=10) == 0
    for s in (idle, waiting, subscriber):
        assert s.recv(100) == b''
        s.close()


# A client that sends requests, but doesn't read the replies, can't hold
# up the server's stop for long. (./run gives Shoal a grace period of a
# second; Valkey doesn't wait at all.)
def test_stop_despite_client_not_reading(own_server):
    address = own_server.address
    r = redis.Redis(*address)
    r.set('test_stop_big', 'x' * 1000000)
    # 100 replies of a megabyte each are far more than the sockets' buffers
    # hold, so the server can't send them all.
    lazy = socket.create_connection(address, timeout=10)
    lazy.sendall(command('GET', 'test_stop_big') * 100)
    with pytest.raises(redis.exceptions.ConnectionError):
        r.execute_command('SHUTDOWN')
    assert own_server.process.wait(timeout=10) == 0
    lazy.close()
