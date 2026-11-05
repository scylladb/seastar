# Shoal's tests connect to a running server, which may be Shoal or, to check
# that the tests themselves are right, a real Redis or Valkey server:
#   pytest --port 6379
# Tests that stop the server start one of their own, with the command given
# by --server-command (without the port), which ./run passes.
import shlex
import socket
import subprocess
import tempfile
import time

import pytest
import redis


def pytest_addoption(parser):
    parser.addoption('--host', default='localhost', help='the server to test')
    parser.addoption('--port', type=int, default=6379, help="the server's port")
    parser.addoption('--server-command',
                     help='the command that starts a server, for tests that stop it')
    parser.addoption('--shoal-version', type=int, default=10,
                     help='the version of Shoal being tested, which decides the '
                          'options of the servers that tests start')


@pytest.fixture(scope='session')
def server(request):
    return request.config.getoption('--host'), request.config.getoption('--port')


@pytest.fixture
def r(server):
    host, port = server
    client = redis.Redis(host=host, port=port)
    yield client
    client.close()


class OwnServer:
    def __init__(self, process, port):
        self.process = process
        self.address = ('localhost', port)


# A server of the test's own, which the test may stop. If it's still running
# when the test ends, it's killed.
@pytest.fixture
def own_server(start_server):
    return start_server()


# A function that starts a server of the test's own, with more options,
# and returns it once it accepts connections. Servers still running when
# the test ends are killed.
@pytest.fixture
def start_server(request):
    command = request.config.getoption('--server-command')
    if not command:
        pytest.skip('needs --server-command')
    # Shoal has --dir since version 10.
    has_dir = request.config.getoption('--shoal-version') >= 10
    started = []

    # The server listens on port, or, if it's None, on a free port.
    def start(*options, port=None):
        if port is None:
            with socket.socket() as s:
                s.bind(('localhost', 0))
                port = s.getsockname()[1]
        if has_dir and '--dir' not in options:
            # A directory of its own for the server's files.
            data = tempfile.TemporaryDirectory()
            started.append((None, data))
            options = (*options, '--dir', data.name)
        log = tempfile.TemporaryFile()
        process = subprocess.Popen(
            [*shlex.split(command), '--port', str(port), *options],
            stdout=log, stderr=subprocess.STDOUT)
        started.append((process, log))
        deadline = time.time() + 30
        while True:
            if process.poll() is not None or time.time() >= deadline:
                # Say why: the server's own output, such as a port in use.
                log.seek(0)
                output = log.read().decode(errors='replace')
                status = ('exited with status ' + str(process.returncode)
                          if process.poll() is not None else 'still not accepting')
                pytest.fail(f'server did not start ({status}):\n{output[-3000:]}')
            try:
                socket.create_connection(('localhost', port), timeout=1).close()
                return OwnServer(process, port)
            except OSError:
                time.sleep(0.1)
    start.is_valkey = 'valkey-server' in command
    yield start
    # Servers still running are stopped as a user would stop them, with
    # SIGTERM, so that they exit normally: a killed server doesn't write
    # what it writes as it exits, such as its code coverage. One that
    # doesn't stop within 10 seconds is killed.
    for process, resource in started:
        if process and process.poll() is None:
            process.terminate()
    for process, resource in started:
        if process:
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
    for process, resource in started:
        if hasattr(resource, 'cleanup'):
            resource.cleanup()
        else:
            resource.close()
