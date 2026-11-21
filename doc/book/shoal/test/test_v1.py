# Tests for the commands of Shoal version 1: PING, ECHO, GET, SET, DEL and
# EXISTS, and for the protocol itself.
import socket
import threading
import uuid

import pytest
import redis


def unique_key():
    return 'key-' + uuid.uuid4().hex


# Check that PING answers PONG, or its argument. (redis-py turns PING's
# reply into a boolean, so the argument is checked on a raw connection.)
def test_ping(r, server):
    assert r.ping()
    assert raw_exchange(server, b'*2\r\n$4\r\nPING\r\n$5\r\nhello\r\n') == b'$5\r\nhello\r\n'


# Check that ECHO answers its argument.
def test_echo(r):
    assert r.echo('hi there') == b'hi there'


# Check that a value written with SET can be read back with GET, and that
# SET replaces an existing value.
def test_set_get(r):
    key = unique_key()
    assert r.set(key, 'hello')
    assert r.get(key) == b'hello'
    assert r.set(key, 'world')
    assert r.get(key) == b'world'


# Check that GET of a key that doesn't exist returns nothing.
def test_get_missing(r):
    assert r.get(unique_key()) is None


# Check that values are binary-safe: any bytes, including line endings and
# zero bytes, come back unchanged.
def test_binary_value(r):
    key = unique_key()
    value = bytes(range(256)) + b'\r\n\x00end'
    r.set(key, value)
    assert r.get(key) == value


# Check a value much larger than what arrives in one read from the network.
def test_large_value(r):
    key = unique_key()
    value = b'x' * 3_000_000
    r.set(key, value)
    assert r.get(key) == value


# Check that DEL deletes the given keys, and counts the ones that existed.
def test_del(r):
    a, b, missing = unique_key(), unique_key(), unique_key()
    r.set(a, '1')
    r.set(b, '2')
    assert r.delete(a, b, missing) == 2
    assert r.get(a) is None and r.get(b) is None
    assert r.delete(a) == 0


# Check that EXISTS counts the given keys that exist, counting a key again
# each time it is repeated.
def test_exists(r):
    key = unique_key()
    assert r.exists(key) == 0
    r.set(key, 'v')
    assert r.exists(key, unique_key(), key) == 2


# Check that command names are case-insensitive.
def test_case_insensitive(r):
    assert r.execute_command('pInG') is True


# Check that a command with the wrong number of arguments is refused.
def test_wrong_number_of_arguments(r):
    with pytest.raises(redis.ResponseError, match='wrong number of arguments'):
        r.execute_command('GET')
    with pytest.raises(redis.ResponseError, match='wrong number of arguments'):
        r.execute_command('SET', unique_key())


# Check that an unknown command is refused, and the connection still works.
def test_unknown_command(r):
    with pytest.raises(redis.ResponseError, match='unknown command'):
        r.execute_command('NOSUCHCOMMAND', 'x')
    assert r.ping()


# Check pipelining: many requests sent at once, before reading any reply,
# get their replies in order.
def test_pipeline(r):
    keys = [unique_key() for _ in range(1000)]
    pipe = r.pipeline(transaction=False)
    for i, key in enumerate(keys):
        pipe.set(key, str(i))
    for key in keys:
        pipe.get(key)
    results = pipe.execute()
    assert results[:1000] == [True] * 1000
    assert results[1000:] == [str(i).encode() for i in range(1000)]


def raw_exchange(server, data):
    """Sends raw bytes to the server, and returns everything it sends back
    within a short time."""
    with socket.create_connection(server, timeout=2) as s:
        s.sendall(data)
        s.shutdown(socket.SHUT_WR)
        reply = b''
        while chunk := s.recv(4096):
            reply += chunk
        return reply


# Check "inline" requests, as typed into nc: words separated by spaces,
# ending with a line ending, without RESP's lengths.
def test_inline_requests(server):
    key = unique_key().encode()
    reply = raw_exchange(server, b'PING\r\nSET ' + key + b' 42\r\nGET ' + key + b'\r\n')
    assert reply == b'+PONG\r\n+OK\r\n$2\r\n42\r\n'


# Check that a request that breaks the protocol gets an error reply, and
# that the server then closes the connection.
def test_protocol_error(server):
    reply = raw_exchange(server, b'*2\r\n$3\r\nGET\r\n$abc\r\n')
    assert reply.startswith(b'-ERR Protocol error')


# Check that the server serves many clients at the same time.
def test_many_clients(server):
    host, port = server
    errors = []
    def client(i):
        try:
            c = redis.Redis(host=host, port=port)
            key = unique_key()
            for j in range(100):
                c.set(key, f'{i}-{j}')
                assert c.get(key) == f'{i}-{j}'.encode()
            c.close()
        except Exception as e:
            errors.append(e)
    threads = [threading.Thread(target=client, args=(i,)) for i in range(20)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    assert errors == []
