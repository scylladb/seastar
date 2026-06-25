# WebSocket protocol implementation

Seastar includes an experimental implementation of a WebSocket server.
Refs:
https://datatracker.ietf.org/doc/html/rfc6455
https://developer.mozilla.org/en-US/docs/Web/API/WebSockets_API/Writing_WebSocket_servers

## Handlers

A WebSocket server needs a user-defined handler in order to be functional. WebSocket specification defines a concept of subprotocols, and Seastar WebSocket server allows registering a single handler per subprotocol.

Each subprotocol has a unique name and is expected to be sent by the connecting client during handshake,
by sending a `Sec-Websocket-Protocol` header with the chosen value.

Aside from specifying the chosen subprotocol name for a handler, the developer is expected to provide a function
which handles the incoming stream of data and returns responses into the output stream.

Here's an example of how to register a simple echo protocol:

```cpp
using namespace seastar;
static experimental::websocket::server ws;
ws.register_handler("echo", [] (input_stream<char>& in, output_stream<char>& out) -> future<> {
    while (true) {
        auto buf = co_await in.read();
        if (buf.empty()) {
            co_return;
        }
        co_await out.write(std::move(buf));
        co_await out.flush();
    }
});
```

Note: the developers should assume that the input stream provides decoded and unmasked data - so the stream should be treated as if it was backed by a TCP socket. Similarly, responses should be sent to the output stream as is, and the WebSocket server implementation will handle its proper serialization, masking and so on.

## Connection lifecycle

The handler owns the normal WebSocket connection lifecycle. To actively close a
connection, close the output stream passed to the handler:

```cpp
ws.register_handler("echo", [] (input_stream<char>& in, output_stream<char>& out) -> future<> {
    while (true) {
        auto buf = co_await in.read();
        if (buf.empty()) {
            break;
        }
        co_await out.write(std::move(buf));
        co_await out.flush();
    }
    co_await out.close();
});
```

On EOF or after the connection is torn down, `in.read()` returns an empty
buffer. If WebSocket frame parsing fails, the current `in.read()` fails with
`websocket::exception`; after the connection teardown completes, later reads
return an empty buffer.

The frame parser enforces RFC 6455 control-frame limits: CLOSE, PING and PONG
frames must not be fragmented and their payload is limited to 125 bytes. Data
frames are limited to 16 MiB by default to avoid allocating memory from
untrusted frame lengths; the limit is configurable via
`connection_options::max_payload_length` passed to the server/client
constructor. A frame that exceeds either limit fails the active `in.read()`
with `websocket::exception`.

Outgoing frames are serialized internally. If an output write or close fails,
the connection's send path is considered failed: later writes fail with the same
exception until the connection is torn down. Applications should treat any
failed output operation as terminal for that WebSocket connection.

When the application initiates the close handshake (by closing the output
stream from inside the handler, or `websocket::client::close()` from outside),
the implementation waits up to `connection_options::close_timeout` (200 ms by
default) for the peer's CLOSE response before tearing down the streams.

For `websocket::client`, `close()` initiates a WebSocket CLOSE handshake and
tears down the connection's streams; the stream teardown shuts down the socket
read side, so a handler blocked in `in.read()` is unblocked and `close()`
resolves once the processing task has finished.

`close()` may fail to make progress only when the outbound path itself stalls
(for example, the peer's TCP receive window is full so the CLOSE frame can
never be sent). In that case `shutdown()` provides an out-of-band escape hatch
that synchronously shuts the connection's input down so the handler can return
and `close()` can complete:

```cpp
ws_client.shutdown();   // only needed when the outbound is stalled
co_await ws_client.close();
```

`websocket::server::stop()` stops accepting new connections and tears down
active connections. It does not initiate a WebSocket CLOSE handshake for those
active connections.

## Error handling

Registered WebSocket handlers can throw arbitrary exceptions during their operation. Currently, exceptions that aren't explicitly handled within the handler will cause the established WebSocket connection to be terminated, and a proper error message will be logged.

## Secure WebSocket (wss://)

Implementation of Secure WebSocket standard, based on HTTPS is currently work in advanced progress. Once reviewed and merged, this section will contain documentation for it.
Ref: https://github.com/scylladb/seastar/pull/1044
