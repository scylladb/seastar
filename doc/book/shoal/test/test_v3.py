# Tests for Shoal version 3, whose request reader takes a fast path when a
# request has arrived whole, and a slow path when it must wait for the rest.
import socket
import time
import uuid


# Check requests that arrive in two pieces, split at every possible point,
# so that the reader must wait for the rest of a line or of a string at
# every position in the request.
def test_request_split_anywhere(server):
    key = ('key-' + uuid.uuid4().hex).encode()
    request = (b'*3\r\n$3\r\nSET\r\n$%d\r\n%s\r\n$5\r\nhello\r\n' % (len(key), key)
               + b'*2\r\n$3\r\nGET\r\n$%d\r\n%s\r\n' % (len(key), key))
    for split in range(1, len(request)):
        with socket.create_connection(server, timeout=5) as s:
            s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            s.sendall(request[:split])
            time.sleep(0.005)
            s.sendall(request[split:])
            s.shutdown(socket.SHUT_WR)
            reply = b''
            while chunk := s.recv(4096):
                reply += chunk
        assert reply == b'+OK\r\n$5\r\nhello\r\n', f'split at {split}'
