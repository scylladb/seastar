# Tests for the commands added in Shoal version 5: EXPIRE, TTL, PERSIST, and
# the EX and PX options of SET.
import time
import uuid

import pytest
import redis


def unique_key():
    return 'key-' + uuid.uuid4().hex


# Check that TTL is -2 for a key that doesn't exist, and -1 for a key that
# has no expiration time.
def test_ttl_without_expiration(r):
    k = unique_key()
    assert r.ttl(k) == -2
    r.set(k, 'v')
    assert r.ttl(k) == -1


# Check that EXPIRE sets an expiration time, which TTL reports in seconds,
# rounded to the nearest second.
def test_expire_and_ttl(r):
    k = unique_key()
    r.set(k, 'v')
    assert r.expire(k, 100) is True
    assert r.ttl(k) == 100


# Check that EXPIRE of a key that doesn't exist returns 0, and doesn't
# create the key.
def test_expire_missing_key(r):
    k = unique_key()
    assert r.expire(k, 100) is False
    assert r.exists(k) == 0


# Check that a key disappears when its expiration time passes: GET, EXISTS
# and TTL all find it gone.
def test_key_expires(r):
    k = unique_key()
    r.set(k, 'v', px=200)
    assert r.get(k) == b'v'
    time.sleep(0.4)
    assert r.get(k) is None
    assert r.exists(k) == 0
    assert r.ttl(k) == -2


# Check that DEL and EXPIRE treat a key that has expired as missing.
def test_expired_key_is_missing(r):
    k1 = unique_key()
    k2 = unique_key()
    r.set(k1, 'v', px=100)
    r.set(k2, 'v', px=100)
    time.sleep(0.3)
    assert r.delete(k1) == 0
    assert r.expire(k2, 100) is False


# Check that KEYS doesn't list keys that have expired.
def test_keys_skips_expired(r):
    p = unique_key() + '-'
    r.set(p + 'a', 'v')
    r.set(p + 'b', 'v', px=100)
    time.sleep(0.3)
    assert r.keys(p + '*') == [(p + 'a').encode()]


# Check that EXPIRE with a time of zero, or a negative time, deletes the key
# at once, and returns 1.
def test_expire_non_positive_deletes(r):
    for seconds in (0, -5):
        k = unique_key()
        r.set(k, 'v')
        assert r.expire(k, seconds) is True
        assert r.exists(k) == 0


# Check that PERSIST removes a key's expiration time and returns 1, and
# returns 0 when the key has no expiration time or doesn't exist.
def test_persist(r):
    k = unique_key()
    r.set(k, 'v', ex=100)
    assert r.persist(k) is True
    assert r.ttl(k) == -1
    assert r.persist(k) is False
    assert r.persist(unique_key()) is False


# Check that a SET without EX or PX removes the key's old expiration time.
def test_set_removes_expiration(r):
    k = unique_key()
    r.set(k, 'v', ex=100)
    r.set(k, 'w')
    assert r.ttl(k) == -1


# Check SET's EX option (in any case) and PX option.
def test_set_ex_px(r):
    k = unique_key()
    assert r.execute_command('SET', k, 'v', 'ex', '100') is True
    assert r.ttl(k) == 100
    assert r.execute_command('SET', k, 'v', 'PX', '2500') is True
    assert r.ttl(k) in (2, 3)
    assert r.get(k) == b'v'


# Check SET's errors for a bad expiration time: not an integer, zero or
# negative, missing, given twice, and an unknown option.
def test_set_expiration_errors(r):
    k = unique_key()
    with pytest.raises(redis.ResponseError, match='not an integer'):
        r.execute_command('SET', k, 'v', 'EX', 'abc')
    for bad in ('0', '-1'):
        with pytest.raises(redis.ResponseError, match='invalid expire time'):
            r.execute_command('SET', k, 'v', 'EX', bad)
    with pytest.raises(redis.ResponseError, match='syntax error'):
        r.execute_command('SET', k, 'v', 'EX')
    with pytest.raises(redis.ResponseError, match='syntax error'):
        r.execute_command('SET', k, 'v', 'EX', '5', 'PX', '100')
    with pytest.raises(redis.ResponseError, match='syntax error'):
        r.execute_command('SET', k, 'v', 'FOO')
    assert r.exists(k) == 0


# Check EXPIRE's error for a time that isn't an integer.
def test_expire_not_integer(r):
    k = unique_key()
    r.set(k, 'v')
    with pytest.raises(redis.ResponseError, match='not an integer'):
        r.execute_command('EXPIRE', k, 'abc')
    assert r.ttl(k) == -1


# Check that TTL and PERSIST need exactly one argument.
def test_ttl_persist_wrong_number_of_arguments(r):
    for command in ('TTL', 'PERSIST'):
        with pytest.raises(redis.ResponseError, match='wrong number of arguments'):
            r.execute_command(command)
        with pytest.raises(redis.ResponseError, match='wrong number of arguments'):
            r.execute_command(command, 'a', 'b')
