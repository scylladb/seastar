# WebSocket protocol implementation

Seastar includes an experimental implementation of a WebSocket server. See
[RFC 6455](https://datatracker.ietf.org/doc/html/rfc6455) and MDN's guide to
[writing WebSocket servers](https://developer.mozilla.org/en-US/docs/Web/API/WebSockets_API/Writing_WebSocket_servers).

## Handlers

A WebSocket server needs a user-defined handler to be functional. The WebSocket specification defines subprotocols, and the Seastar WebSocket server allows one handler to be registered per subprotocol.

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

## Error handling

Registered WebSocket handlers can throw arbitrary exceptions during their operation. Currently, exceptions that aren't explicitly handled within the handler will cause the established WebSocket connection to be terminated, and a proper error message will be logged.

## Secure WebSocket (`wss://`)

The WebSocket server does not currently support secure WebSocket connections (`wss://`).
