# Shoal's tests connect to a running server, which may be Shoal or, to check
# that the tests themselves are right, a real Redis or Valkey server:
#   pytest --port 6379
import pytest
import redis


def pytest_addoption(parser):
    parser.addoption('--host', default='localhost', help='the server to test')
    parser.addoption('--port', type=int, default=6379, help="the server's port")


@pytest.fixture(scope='session')
def server(request):
    return request.config.getoption('--host'), request.config.getoption('--port')


@pytest.fixture
def r(server):
    host, port = server
    client = redis.Redis(host=host, port=port)
    yield client
    client.close()
