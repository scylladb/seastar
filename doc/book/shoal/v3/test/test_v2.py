# Tests for the commands added in Shoal version 2: KEYS and FLUSHALL.
import threading
import uuid

import pytest
import redis


def unique_prefix():
    return 'prefix-' + uuid.uuid4().hex + '-'


# Check KEYS with each kind of pattern element: '*', '?', sets of
# characters, ranges, negated sets, and escaped special characters.
def test_keys_patterns(r):
    p = unique_prefix()
    names = ['a', 'b', 'c', 'ab', 'abc', 'x*y', 'xzy']
    for name in names:
        r.set(p + name, '1')
    def keys(pattern):
        return sorted(k.decode()[len(p):] for k in r.keys(p + pattern))
    assert keys('*') == sorted(names)
    assert keys('?') == ['a', 'b', 'c']
    assert keys('a*') == ['a', 'ab', 'abc']
    assert keys('*b*') == ['ab', 'abc', 'b']
    assert keys('[ab]') == ['a', 'b']
    assert keys('[a-b]') == ['a', 'b']
    assert keys('[^a]') == ['b', 'c']
    assert keys('x?y') == ['x*y', 'xzy']
    assert keys('x\\*y') == ['x*y']
    assert keys('abcd') == []


# Check that KEYS needs exactly one argument.
def test_keys_wrong_number_of_arguments(r):
    with pytest.raises(redis.ResponseError, match='wrong number of arguments'):
        r.execute_command('KEYS')
    with pytest.raises(redis.ResponseError, match='wrong number of arguments'):
        r.execute_command('KEYS', 'a', 'b')


# Check that KEYS, while another client adds and deletes keys, returns
# each key that exists all along exactly once. A server that yields during
# KEYS does so many times while it looks at the 100,000 other keys, and
# the other client adds enough keys to make a hash table grow.
def test_keys_while_changing(r, server):
    p = unique_prefix()
    stable = {f'{p}stable-{i}' for i in range(1000)}
    pipe = r.pipeline(transaction=False)
    for key in stable:
        pipe.set(key, '1')
    for i in range(100_000):
        pipe.set(f'{p}other-{i}', '1')
    pipe.execute()
    done = False
    errors = []
    def change():
        try:
            c = redis.Redis(host=server[0], port=server[1])
            for i in range(0, 100_000, 1000):
                pipe = c.pipeline(transaction=False)
                for j in range(i, i + 1000):
                    pipe.set(f'{p}new-{j}', '1')
                    pipe.set(f'{p}newer-{j}', '1')
                    pipe.delete(f'{p}other-{j}')
                pipe.execute()
            c.close()
        except Exception as e:
            errors.append(e)
        nonlocal done
        done = True
    thread = threading.Thread(target=change)
    thread.start()
    try:
        while not done:
            found = [k.decode() for k in r.keys(p + 'stable-*')]
            assert len(found) == len(set(found))
            assert set(found) == stable
    finally:
        thread.join()
    assert errors == []
    # Delete the 200,000 keys that this test leaves.
    r.flushall()


# Check that FLUSHALL deletes every key. This deletes the keys of other
# tests too, but they each use keys of their own, and don't run at the
# same time.
def test_flushall(r):
    p = unique_prefix()
    for i in range(100):
        r.set(f'{p}{i}', 'x')
    assert r.flushall()
    assert r.get(f'{p}0') is None
    assert r.keys('*') == []
    assert r.exists(*[f'{p}{i}' for i in range(100)]) == 0
    r.set(f'{p}0', 'y')
    assert r.get(f'{p}0') == b'y'


# Check FLUSHALL's optional argument, ASYNC or SYNC, in any case.
def test_flushall_argument(r):
    assert r.execute_command('FLUSHALL', 'async') is True
    assert r.execute_command('FLUSHALL', 'SYNC') is True
    with pytest.raises(redis.ResponseError, match='syntax error'):
        r.execute_command('FLUSHALL', 'bogus')
    with pytest.raises(redis.ResponseError, match='syntax error'):
        r.execute_command('FLUSHALL', 'async', 'sync')
