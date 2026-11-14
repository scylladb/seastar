# Tests for Shoal version 4: Pub/Sub (SUBSCRIBE, UNSUBSCRIBE, PUBLISH) and
# FLUSHALL ASYNC. Like all of Shoal's tests, they pass against Valkey too.
import socket
import time
import uuid

import pytest
import redis


def unique(prefix):
    return f'{prefix}-{uuid.uuid4().hex}'


def command(*args):
    out = b'*%d\r\n' % len(args)
    for a in args:
        a = a if isinstance(a, bytes) else a.encode()
        out += b'$%d\r\n%s\r\n' % (len(a), a)
    return out


def bulk(s):
    s = s if isinstance(s, bytes) else s.encode()
    return b'$%d\r\n%s\r\n' % (len(s), s)


def confirmation(kind, channel, count):
    return b'*3\r\n' + bulk(kind) + (bulk(channel) if channel is not None else b'$-1\r\n') + b':%d\r\n' % count


def read_exactly(s, n):
    data = b''
    while len(data) < n:
        chunk = s.recv(n - len(data))
        assert chunk, 'connection closed'
        data += chunk
    return data


def exchange(server, *commands, expect):
    with socket.create_connection(server, timeout=5) as s:
        s.sendall(b''.join(command(*c) for c in commands))
        assert read_exactly(s, len(expect)) == expect


def get_message(p, timeout=5):
    deadline = time.time() + timeout
    while time.time() < deadline:
        m = p.get_message(timeout=0.1)
        if m is not None:
            return m
    return None


# Check that a subscriber receives what is published to its channel, and
# that PUBLISH counts it.
def test_publish_to_subscriber(r):
    channel = unique('ch')
    p = r.pubsub()
    p.subscribe(channel)
    assert get_message(p)['type'] == 'subscribe'
    assert r.publish(channel, 'hello') == 1
    m = get_message(p)
    assert m['type'] == 'message'
    assert m['channel'] == channel.encode()
    assert m['data'] == b'hello'
    p.close()


# Check that PUBLISH to a channel nobody subscribed to returns 0, and that a
# subscriber to another channel doesn't get the message.
def test_publish_without_subscribers(r):
    p = r.pubsub()
    other = unique('other')
    p.subscribe(other)
    assert get_message(p)['type'] == 'subscribe'
    assert r.publish(unique('ch'), 'lost') == 0
    assert get_message(p, timeout=0.3) is None
    p.close()


# Check that a message may contain any bytes.
def test_publish_binary_message(r):
    channel = unique('ch')
    p = r.pubsub()
    p.subscribe(channel)
    get_message(p)
    message = bytes(range(256)) * 4 + b'\r\n*3\r\n'
    assert r.publish(channel, message) == 1
    assert get_message(p)['data'] == message
    p.close()


# Check that every subscriber gets the message, and PUBLISH counts them all.
def test_many_subscribers(r):
    channel = unique('ch')
    subscribers = [r.pubsub() for _ in range(20)]
    for p in subscribers:
        p.subscribe(channel)
        assert get_message(p)['type'] == 'subscribe'
    assert r.publish(channel, 'to all') == 20
    for p in subscribers:
        assert get_message(p)['data'] == b'to all'
        p.close()


# Check that a subscriber receives messages in the order they were
# published.
def test_message_order(r):
    channel = unique('ch')
    p = r.pubsub()
    p.subscribe(channel)
    get_message(p)
    for i in range(1000):
        r.publish(channel, str(i))
    for i in range(1000):
        assert get_message(p)['data'] == str(i).encode()
    p.close()


# Check the exact confirmations of SUBSCRIBE and UNSUBSCRIBE: one for each
# channel, with the number of channels the client is subscribed to.
def test_subscribe_confirmations(server):
    a, b = unique('a'), unique('b')
    exchange(server, ('SUBSCRIBE', a, b), ('UNSUBSCRIBE', a), ('UNSUBSCRIBE', b),
             expect=confirmation('subscribe', a, 1) + confirmation('subscribe', b, 2)
             + confirmation('unsubscribe', a, 1) + confirmation('unsubscribe', b, 0))


# Check that subscribing twice to the same channel doesn't count it twice.
def test_subscribe_same_channel_twice(server):
    c = unique('c')
    exchange(server, ('SUBSCRIBE', c, c),
             expect=confirmation('subscribe', c, 1) + confirmation('subscribe', c, 1))


# Check that UNSUBSCRIBE without arguments unsubscribes from every channel.
def test_unsubscribe_all(server):
    a, b = unique('a'), unique('b')
    with socket.create_connection(server, timeout=5) as s:
        s.sendall(command('SUBSCRIBE', a, b))
        read_exactly(s, len(confirmation('subscribe', a, 1) + confirmation('subscribe', b, 2)))
        s.sendall(command('UNSUBSCRIBE'))
        expect_a = confirmation('unsubscribe', a, 1)
        reply = read_exactly(s, len(expect_a) + len(confirmation('unsubscribe', b, 0)))
        # The order of the channels isn't defined.
        assert reply in (confirmation('unsubscribe', a, 1) + confirmation('unsubscribe', b, 0),
                         confirmation('unsubscribe', b, 1) + confirmation('unsubscribe', a, 0))


# Check UNSUBSCRIBE when the client has no subscriptions: without arguments,
# the confirmation has a null channel; with one, it names the channel.
def test_unsubscribe_without_subscriptions(server):
    x = unique('x')
    exchange(server, ('UNSUBSCRIBE',), ('UNSUBSCRIBE', x),
             expect=confirmation('unsubscribe', None, 0) + confirmation('unsubscribe', x, 0))


# Check that a subscribed client's PING gets an array, not +PONG.
def test_ping_while_subscribed(server):
    c = unique('c')
    exchange(server, ('SUBSCRIBE', c), ('PING',), ('PING', 'hi'),
             expect=confirmation('subscribe', c, 1)
             + b'*2\r\n' + bulk('pong') + bulk('')
             + b'*2\r\n' + bulk('pong') + bulk('hi'))


# Check that a subscribed client can't send other commands, and that it can
# again once it unsubscribes from everything.
def test_commands_while_subscribed(server):
    c, key = unique('c'), unique('key')
    error = (b"-ERR Can't execute 'get': only (P|S)SUBSCRIBE / (P|S)UNSUBSCRIBE / "
             b"PING / QUIT / RESET are allowed in this context\r\n")
    exchange(server, ('SUBSCRIBE', c), ('GET', key), ('UNSUBSCRIBE',), ('GET', key),
             expect=confirmation('subscribe', c, 1) + error
             + confirmation('unsubscribe', c, 0) + b'$-1\r\n')


# Check the wrong numbers of arguments.
def test_pubsub_wrong_arguments(server):
    exchange(server, ('SUBSCRIBE',), ('PUBLISH', 'ch'), ('PUBLISH', 'ch', 'a', 'b'),
             expect=b"-ERR wrong number of arguments for 'subscribe' command\r\n"
             + b"-ERR wrong number of arguments for 'publish' command\r\n"
             + b"-ERR wrong number of arguments for 'publish' command\r\n")


# Check that a subscriber that disconnects stops being counted.
def test_subscriber_disconnects(r, server):
    channel = unique('ch')
    s = socket.create_connection(server, timeout=5)
    s.sendall(command('SUBSCRIBE', channel))
    read_exactly(s, len(confirmation('subscribe', channel, 1)))
    assert r.publish(channel, 'one') == 1
    s.close()
    deadline = time.time() + 5
    while r.publish(channel, 'two') != 0:
        assert time.time() < deadline, 'the subscriber is still counted'
        time.sleep(0.01)


# Check that a subscriber that never reads doesn't hold up the publisher.
# The server may disconnect it rather than keep its messages without limit
# (Shoal does after 32 MB, by default; Valkey's limit is also 32 MB, but it
# didn't disconnect this subscriber in 100 MB), but once it disconnects it,
# the subscriber isn't counted anymore.
def test_slow_subscriber(r, server):
    channel = unique('ch')
    s = socket.create_connection(server, timeout=5)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
    s.sendall(command('SUBSCRIBE', channel))
    read_exactly(s, len(confirmation('subscribe', channel, 1)))
    message = b'x' * (1 << 20)
    start = time.time()
    counts = []
    for _ in range(100):
        counts.append(r.publish(channel, message))
    assert time.time() - start < 10
    assert counts[0] == 1
    if 0 in counts:
        assert set(counts[counts.index(0):]) == {0}
    s.close()


# Check that FLUSHALL ASYNC deletes all the keys, and replies OK.
def test_flushall_async(r):
    for i in range(1000):
        r.set(unique('key'), 'value')
    assert r.flushall(asynchronous=True)
    assert r.keys('*') == []


# Check that FLUSHALL ASYNC deletes only the keys that existed when it ran:
# a key set right after it survives.
def test_flushall_async_keeps_new_keys(server):
    key = unique('key')
    exchange(server, ('SET', unique('old'), 'v'), ('FLUSHALL', 'ASYNC'),
             ('SET', key, 'new'), ('GET', key),
             expect=b'+OK\r\n+OK\r\n+OK\r\n' + bulk('new'))


# Check FLUSHALL's options, which are case-insensitive.
def test_flushall_options(server):
    exchange(server, ('FLUSHALL', 'async'), ('FLUSHALL', 'Sync'), ('FLUSHALL', 'bad'),
             ('FLUSHALL', 'ASYNC', 'SYNC'),
             expect=b'+OK\r\n+OK\r\n-ERR syntax error\r\n-ERR syntax error\r\n')
