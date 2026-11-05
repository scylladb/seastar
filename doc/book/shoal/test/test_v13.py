# Tests for Shoal version 13, which has an admin API over HTTP and HTTPS.
# Valkey has no such API, so these tests run only against Shoal.
import json
import ssl
import urllib.error
import urllib.request

import pytest
import redis

from test_v9 import free_port, make_certificate


def admin_server(start_server, *options):
    """Starts a Shoal with an admin API over HTTP, and returns a Redis
    client and the API's base URL."""
    if start_server.is_valkey:
        pytest.skip('Valkey has no admin API')
    port = free_port()
    srv = start_server('--admin-port', str(port), *options)
    return redis.Redis(port=srv.address[1]), f'http://localhost:{port}'


def call(url, method='GET', context=None):
    """Calls the API, and returns the HTTP status and the decoded JSON
    reply, or None if the reply is empty."""
    request = urllib.request.Request(url, method=method)
    try:
        with urllib.request.urlopen(request, timeout=10,
                                    context=context) as reply:
            status, body = reply.status, reply.read()
    except urllib.error.HTTPError as e:
        status, body = e.code, e.read()
    return status, json.loads(body) if body else None


# Check that /v1/info and /v1/shards count the keys and their memory, on
# all the shards together and on each.
def test_info(start_server):
    r, api = admin_server(start_server)
    r.mset({f'info:{i}': 'x' for i in range(100)})
    status, info = call(f'{api}/v1/info')
    assert status == 200
    assert info['keys'] == 100 and info['used_memory'] > 0
    assert info['maxmemory'] == 0 and info['evicted_keys'] == 0
    status, shards = call(f'{api}/v1/shards')
    assert status == 200
    assert len(shards) == info['shards']
    assert [s['shard'] for s in shards] == list(range(info['shards']))
    assert sum(s['keys'] for s in shards) == 100


# Check that /v1/keys lists the keys that match a pattern, as KEYS does,
# and all of them without one, also when there are many.
def test_keys(start_server):
    r, api = admin_server(start_server)
    r.mset({f'keys:{i}': 'x' for i in range(20000)})
    r.set('other', 'x')
    status, keys = call(f'{api}/v1/keys?pattern=keys:1*')
    assert status == 200
    assert sorted(keys) == sorted(k.decode() for k in r.keys('keys:1*'))
    status, keys = call(f'{api}/v1/keys')
    assert status == 200 and len(keys) == 20001


# Check that DELETE /v1/keys/{key} deletes the key, for Redis clients too,
# and answers 404 for a key that doesn't exist.
def test_delete_key(start_server):
    r, api = admin_server(start_server)
    r.set('doomed', 'x')
    assert call(f'{api}/v1/keys/doomed', 'DELETE')[0] == 200
    assert r.exists('doomed') == 0
    status, error = call(f'{api}/v1/keys/doomed', 'DELETE')
    assert status == 404 and error['code'] == 404


# Check that POST /v1/rewrite starts a rewrite of the logs, as BGREWRITEAOF
# does, and that the data survives it.
def test_rewrite(start_server):
    r, api = admin_server(start_server)
    r.set('kept', 'x')
    assert call(f'{api}/v1/rewrite', 'POST')[0] in (200, 409)
    assert r.get('kept') == b'x'


# Check that an unknown path, or a method that the API doesn't have for a
# path, gets 404.
def test_unknown(start_server):
    r, api = admin_server(start_server)
    assert call(f'{api}/v1/nothing')[0] == 404
    assert call(f'{api}/v1/info', 'DELETE')[0] == 404


# Check that --admin-tls-port serves the API over HTTPS, with the
# certificate of --tls-cert-file, while --admin-port goes on serving it
# over HTTP.
def test_https(start_server, tmp_path):
    cert, key = make_certificate(tmp_path, 'server')
    tls_port = free_port()
    r, api = admin_server(start_server, '--admin-tls-port', str(tls_port),
                          '--tls-cert-file', cert, '--tls-key-file', key,
                          '--tls-auth-clients', 'no')
    r.set('secret', 'x')
    context = ssl.create_default_context(cafile=cert)
    status, info = call(f'https://localhost:{tls_port}/v1/info',
                        context=context)
    assert status == 200 and info['keys'] == 1
    assert call(f'{api}/v1/info')[1]['keys'] == 1
