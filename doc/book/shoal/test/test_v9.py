# Tests for Shoal version 9: requests split anywhere between reads, a huge
# pipeline sent before any reply is read, large replies, and the options
# of the listening socket and of the connections. Like all of Shoal's
# tests, they pass against Valkey too.
import os
import shutil
import socket
import ssl
import subprocess
import time
import uuid

import pytest
import redis


def unique_key():
    return 'key-' + uuid.uuid4().hex


def command(*args):
    out = b'*%d\r\n' % len(args)
    for a in args:
        a = a.encode() if isinstance(a, str) else a
        out += b'$%d\r\n%s\r\n' % (len(a), a)
    return out


def read_replies(s, n):
    """Reads n replies that are integers or simple strings, one line each,
    and returns them."""
    data = b''
    while data.count(b'\r\n') < n:
        more = s.recv(1 << 20)
        assert more, 'the server closed the connection'
        data += more
    lines = data.split(b'\r\n')
    assert lines[n] == b''
    return lines[:n]


# Check that a client that sends a huge pipeline, and reads the replies
# only after sending all of it, gets them all (as in chapter 4's exercise
# 6, which used APPEND):
# the server keeps reading while the replies wait for the client.
def test_huge_pipeline(server):
    keys = [unique_key() for _ in range(10)]
    n = 1_000_000
    pipeline = b''.join(command('RPUSH', keys[i % 10], 'x')
                        for i in range(n))
    with socket.create_connection(server, timeout=30) as s:
        s.sendall(pipeline)
        replies = read_replies(s, n)
    assert replies[-10:] == [b':%d' % (n // 10)] * 10
    r = redis.Redis(host=server[0], port=server[1])
    r.delete(*keys)


# Check that requests arrive whole however the client splits them, here
# into pieces of 1, 2, ... 7 bytes, so that numbers, line endings and
# arguments are all cut somewhere.
def test_split_everywhere(server):
    key, list_key = unique_key(), unique_key()
    value = 'v' * 100
    data = (command('SET', key, value) + command('GET', key)
            + b'PING\r\n' + command('RPUSH', list_key, 'w'))
    with socket.create_connection(server, timeout=10) as s:
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        i, step = 0, 1
        while i < len(data):
            s.sendall(data[i:i + step])
            i += step
            step = step % 7 + 1
        expected = (b'+OK\r\n$100\r\n' + value.encode() + b'\r\n'
                    + b'+PONG\r\n:1\r\n')
        got = b''
        while len(got) < len(expected):
            more = s.recv(4096)
            assert more
            got += more
    assert got == expected


# Check that KEYS returns every matching key when the reply is much larger
# than one piece of a reply (16 KB).
def test_keys_large_reply(r):
    prefix = unique_key()
    keys = {f'{prefix}-{i}' for i in range(20_000)}
    pipe = r.pipeline(transaction=False)
    for k in keys:
        pipe.set(k, 'v')
    pipe.execute()
    assert {k.decode() for k in r.keys(f'{prefix}-*')} == keys
    r.delete(*keys)


def ss(*args):
    if not shutil.which('ss'):
        pytest.skip('no ss')
    return subprocess.run(['ss', '-H', *args], capture_output=True,
                          text=True, check=True).stdout


# Check that the listening socket's backlog is Valkey's default, 511,
# unless net.core.somaxconn is lower. ss shows it as the Send-Q of the
# listening socket.
def test_backlog(server):
    host, port = server
    with open('/proc/sys/net/core/somaxconn') as f:
        somaxconn = int(f.read())
    lines = ss('-ltn', f'sport = :{port}').splitlines()
    assert lines
    for line in lines:
        assert int(line.split()[2]) == min(511, somaxconn)


# Check that the server turns on TCP keepalive on a client's connection:
# ss shows the keepalive timer of the server's end.
def test_keepalive(server):
    host, port = server
    with socket.create_connection(server, timeout=10) as s:
        s.sendall(command('PING'))
        assert s.recv(100) == b'+PONG\r\n'
        local_port = s.getsockname()[1]
        out = ss('-tno', 'state', 'established',
                 f'( sport = :{port} and dport = :{local_port} )')
    assert 'timer:(keepalive' in out


# Check that INFO counts the clients connected, and the bytes read from the
# clients and written to them.
def test_info(r):
    before = r.info('stats')
    key, value = unique_key(), 'x' * 10_000
    r.set(key, value)
    assert r.get(key) == value.encode()
    after = r.info('stats')
    assert (after['total_net_input_bytes']
            - before['total_net_input_bytes']) >= 10_000
    assert (after['total_net_output_bytes']
            - before['total_net_output_bytes']) >= 10_000
    assert r.info('clients')['connected_clients'] >= 1
    r.delete(key)


def make_certificate(directory, name, ca=None, is_ca=False):
    """Makes a private key and a certificate for localhost, in directory,
    signed by ca, a (certificate, key) pair, or else by itself, and if
    is_ca, one that may sign others. Returns the paths of the certificate
    and the key."""
    if not shutil.which('openssl'):
        pytest.skip('no openssl')
    cert, key = directory / f'{name}.crt', directory / f'{name}.key'
    args = ['openssl', 'req', '-new', '-newkey', 'rsa:2048', '-nodes',
            '-keyout', key, '-out', cert, '-days', '1',
            '-subj', f'/CN=localhost/O={name}',
            '-addext', 'subjectAltName=DNS:localhost']
    if is_ca:
        args += ['-addext', 'basicConstraints=critical,CA:TRUE',
                 '-addext', 'keyUsage=critical,keyCertSign,cRLSign']
    else:
        args += ['-addext', 'basicConstraints=critical,CA:FALSE',
                 '-addext', 'keyUsage=critical,digitalSignature,'
                            'keyEncipherment']
    if ca:
        args += ['-CA', ca[0], '-CAkey', ca[1]]
    else:
        args += ['-x509']
    subprocess.run(args, capture_output=True, check=True)
    return str(cert), str(key)


def free_port():
    with socket.socket() as s:
        s.bind(('localhost', 0))
        return s.getsockname()[1]


def tls_client(port, ca, cert=None):
    """A Redis client of a TLS port, which trusts the certificates signed
    by ca, and presents cert, a (certificate, key) pair, if given."""
    return redis.Redis(port=port, ssl=True, ssl_ca_certs=ca,
                       ssl_certfile=cert[0] if cert else None,
                       ssl_keyfile=cert[1] if cert else None)


def server_organization(port, ca):
    """The organization in the certificate that the server presents."""
    context = ssl.create_default_context(cafile=ca)
    with socket.create_connection(('localhost', port), timeout=10) as s:
        with context.wrap_socket(s, server_hostname='localhost') as t:
            subject = dict(x[0] for x in t.getpeercert()['subject'])
    return subject['organizationName']


# Check that a client can use the TLS port, and that the server presents
# its certificate there, while the TCP port goes on working.
def test_tls(start_server, tmp_path):
    cert, key = make_certificate(tmp_path, 'server')
    port = free_port()
    srv = start_server('--tls-port', str(port), '--tls-cert-file', cert,
                       '--tls-key-file', key, '--tls-auth-clients', 'no')
    # The clients are closed when the test is done with them: a TLS
    # client left connected makes the server wait for it to answer the
    # closing of the connection, for 10 seconds, when it stops.
    with tls_client(port, ca=cert) as r:
        assert r.ping()
        key_name = unique_key()
        assert r.set(key_name, 'v' * 100_000)
        assert r.get(key_name) == b'v' * 100_000
    assert server_organization(port, cert) == 'server'
    with redis.Redis(port=srv.address[1]) as plain:
        assert plain.get(key_name) == b'v' * 100_000


# Check that with --tls-auth-clients yes, the server accepts only clients
# with a certificate signed by its --tls-ca-cert-file.
def test_tls_client_certificates(start_server, tmp_path):
    ca = make_certificate(tmp_path, 'ca', is_ca=True)
    server = make_certificate(tmp_path, 'server', ca)
    client = make_certificate(tmp_path, 'client', ca)
    stranger = make_certificate(tmp_path, 'stranger')
    port = free_port()
    start_server('--tls-port', str(port), '--tls-cert-file', server[0],
                 '--tls-key-file', server[1], '--tls-ca-cert-file', ca[0],
                 '--tls-auth-clients', 'yes')
    with tls_client(port, ca=ca[0], cert=client) as r:
        assert r.ping()
    for cert in (None, stranger):
        with pytest.raises(redis.ConnectionError):
            tls_client(port, ca=ca[0], cert=cert).ping()


# Check that when the certificate's files are replaced, new connections get
# the new certificate, without a restart. Shoal notices the change by
# itself; Valkey reloads the files when CONFIG SET names them (to Shoal,
# an unknown command).
def test_tls_reload(start_server, tmp_path):
    old = make_certificate(tmp_path, 'old')
    (tmp_path / 'new').mkdir()
    new = make_certificate(tmp_path / 'new', 'new')
    cert, key = str(tmp_path / 'server.crt'), str(tmp_path / 'server.key')
    shutil.copy(old[0], cert)
    shutil.copy(old[1], key)
    port = free_port()
    srv = start_server('--tls-port', str(port), '--tls-cert-file', cert,
                       '--tls-key-file', key, '--tls-auth-clients', 'no')
    assert server_organization(port, old[0]) == 'old'
    # Replace the files, each by renaming a complete new file over it.
    for src, dst in ((new[0], cert), (new[1], key)):
        shutil.copy(src, dst + '.tmp')
        os.rename(dst + '.tmp', dst)
    try:
        redis.Redis(port=srv.address[1]).config_set('tls-cert-file', cert)
    except redis.ResponseError:
        pass
    deadline = time.time() + 10
    while True:
        try:
            if server_organization(port, new[0]) == 'new':
                break
        except ssl.SSLError:
            pass    # still the old certificate, which new[0] doesn't trust
        assert time.time() < deadline, 'the certificate was not reloaded'
        time.sleep(0.1)


# Check that clients can connect through a Unix-domain socket, and that a
# socket file left by an earlier server doesn't stop a new one.
def test_unix_socket(start_server, tmp_path):
    path = str(tmp_path / 'server.sock')
    for _ in range(2):
        srv = start_server('--unixsocket', path)
        r = redis.Redis(unix_socket_path=path)
        key = unique_key()
        assert r.set(key, 'hello')
        assert redis.Redis(port=srv.address[1]).get(key) == b'hello'
        r.close()
        srv.process.kill()
        srv.process.wait()


# Check that --bind takes names as well as addresses. (Shoal listens on
# all the addresses of the name, 127.0.0.1 and ::1 for localhost; Valkey
# only on the first.)
def test_bind_name(start_server):
    srv = start_server('--bind', 'localhost')
    assert redis.Redis(host='127.0.0.1', port=srv.address[1]).ping()
