# Tests for Shoal version 15, which has metrics, served in Prometheus's
# format by the admin API, at /metrics. Valkey has no such API, so these
# tests run only against Shoal.
import re
import time
import urllib.request

import pytest
import redis

from test_v9 import free_port
from test_v14 import eventually


def metrics_server(start_server, *options):
    """Starts a Shoal with an admin API, and returns a Redis client, the
    URL of its metrics, and the server."""
    if start_server.is_valkey:
        pytest.skip('Valkey has no metrics API')
    port = free_port()
    srv = start_server('--admin-port', str(port), *options)
    return (redis.Redis(port=srv.address[1]),
            f'http://localhost:{port}/metrics', srv)


line = re.compile(r'^(\w+)(?:\{(.*)\})? (\S+)$')


def metrics(url, query=''):
    """The metrics, as a list of (name, labels, value)."""
    with urllib.request.urlopen(url + query, timeout=10) as reply:
        text = reply.read().decode()
    result = []
    for l in text.splitlines():
        if l.startswith('#') or not l:
            continue
        m = line.match(l)
        assert m, l
        labels = dict(re.findall(r'(\w+)="([^"]*)"', m[2] or ''))
        result.append((m[1], labels, float(m[3])))
    return result


def total(url, name, **labels):
    """The sum of a metric's values on all the shards, of those with these
    labels. A histogram's lines, name_count, name_sum and name_bucket, are
    selected by the histogram's name."""
    family = re.sub(r'_(count|sum|bucket)$', '', name)
    return sum(v for n, l, v in metrics(url, f'?__name__={family}')
               if n == name and all(l.get(k) == v for k, v in labels.items()))


# Check that each command's calls are counted, on all the shards together,
# and that a command that wasn't called isn't reported.
def test_command_calls(start_server):
    r, url, _ = metrics_server(start_server)
    for i in range(5):
        r.set(f'calls:{i}', i)
    for i in range(3):
        r.get(f'calls:{i}')
    assert total(url, 'shoal_commands_calls', command='set') == 5
    assert total(url, 'shoal_commands_calls', command='get') == 3
    assert not [l for n, l, v in metrics(url, '?__name__=commands_calls')
                if l.get('command') == 'lpush']
    # Check that they're summed over the shards, unless asked not to be.
    assert all('shard' not in l
               for n, l, v in metrics(url, '?__name__=commands_calls'))
    per_shard = metrics(url, '?__name__=commands_calls&__aggregate__=false')
    assert all('shard' in l for n, l, v in per_shard)
    assert sum(v for n, l, v in per_shard if l['command'] == 'set') == 5


# Check that the keys and their memory are counted, on every shard.
def test_store_metrics(start_server):
    r, url, _ = metrics_server(start_server)
    r.mset({f'store:{i}': 'x' * 100 for i in range(1000)})
    keys = [v for n, l, v in metrics(url, '?__name__=store_keys')]
    assert len(keys) >= 2 and sum(keys) == 1000
    assert total(url, 'shoal_store_used_memory') > 100000


# Check that the log counts its flushes, and how long they took.
def test_log_metrics(start_server):
    r, url, _ = metrics_server(start_server, '--appendfsync', 'always')
    for i in range(20):
        r.set(f'log:{i}', i)
    flushes = total(url, 'shoal_log_flushes')
    assert flushes > 0
    assert total(url, 'shoal_log_flush_latency_count') == flushes
    assert total(url, 'shoal_log_flush_latency_bucket', le='+Inf') == flushes
    assert total(url, 'shoal_log_bytes_written') >= 20 * 20


# Check that the metrics can be filtered by name and by label, without
# their help lines, and that Seastar's own metrics have Shoal's prefix.
def test_filters(start_server):
    r, url, _ = metrics_server(start_server)
    names = {n for n, l, v in metrics(url, '?__name__=store_keys')}
    assert names == {'shoal_store_keys'}
    shard0 = metrics(url, '?__name__=store_keys&shard=0')
    assert [l['shard'] for n, l, v in shard0] == ['0']
    with urllib.request.urlopen(url + '?__help__=false', timeout=10) as reply:
        assert '# HELP' not in reply.read().decode()
    assert metrics(url, '?__name__=reactor_utilization')


# Check that a primary reports its feed, and that the replica's lag goes
# back to 0 once it fetched everything.
def test_replication_metrics(start_server):
    if start_server.is_valkey:
        pytest.skip('Valkey has no metrics API')
    replication_port = free_port()
    r, url, _ = metrics_server(start_server, '--replication-port',
                               str(replication_port))
    replica = start_server('--replicaof', f'127.0.0.1:{replication_port}')
    rr = redis.Redis(port=replica.address[1])
    r.mset({f'feed:{i}': i for i in range(100)})
    eventually(lambda: rr.dbsize() == 100)
    assert total(url, 'shoal_replication_feed_bytes') > 0
    assert total(url, 'shoal_replication_snapshots') >= 2
    eventually(lambda: total(url, 'shoal_replication_lag_bytes') == 0)
