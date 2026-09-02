# RPC streaming

## Streaming API

### Sink and Source

The basic elements of the streaming API are `rpc::sink` and `rpc::source`. The former
is used to send data and the latter to receive it. The client and server
each have their own pair of sinks and sources. `rpc::sink` and `rpc::source` are
templated classes where template parameters describe a type of the data
that is sent/received. For instance the sink that is used to send messages
containing `int` and `long` will be of a type `rpc::sink<int, long>`.  The
opposite end of the stream will have a source of the type `rpc::source<int, long>`
which will be used to receive those messages. Messages are received at a
source as a `std::optional` containing the actual message as a `std::tuple`. A disengaged
optional means EOS (end of stream): the stream was closed by the peer. If an
error happens before EOS is received, the receiver cannot be sure that it received all
the data.

To send data using `rpc::sink<int, long>`, one can write the following (assuming a `seastar::async` context):

```cpp
      while (has_data()) {
          int data1 = get_data1();
          long data2 = get_data2();
          sink(data1, data2).get(); // sends data
      }
      sink.close().get(); // closes stream
```

To receive:

```cpp
      while (true) {
          std::optional<std::tuple<int, long>> data = source().get();
          if (!data) {
             // unengaged optional means EOS
             break;
          } else {
             auto [data1, data2] = *data;
             // process data
          }
      }
```

### Creating a stream

To open an RPC stream, an RPC client must already exist. The stream
will be associated with the client, and closing the client aborts any stream
still open on it. Aborting a stream this way is not a substitute for closing
it, see [Closing a stream](#closing-a-stream) below. Given RPC client `rc`, and a `serializer` class that models the Serializer concept (as explained in the rpc::protocol class), one creates `rpc::sink` as follows
(again assuming `seastar::async` context):

```cpp
    rpc::sink<int, long> sink = rc.make_stream_sink<serializer, int, long>().get();
```

Now the client has the sink that can be used for streaming data to
a server, but how the server will get a corresponding `rpc::source` to
read it? For that the sink should be passed to the server by an RPC
call. To receive a sink a server should register an RPC handler that will
be used to receive it along with any auxiliary information deemed necessary.
To receive the sink above one may register an RPC handler like that:

```cpp
    rpc_proto.register_handler(1, [] (int aux_data, rpc::source<int, long> source) {
    });
```

Notice that `rpc::sink` is received as an `rpc::source` since at the server
side it will be used to receive data. All that remains is for the client to
invoke this RPC handler with `aux_data` and the sink.

For communication in the other direction, from server to client, the server
must have a sink and the client must have a source. Because messages in this
direction may have a different type from client-to-server messages, the sink
and source may have a different
type as well.

The server initiates creation of a communication channel in the other direction.
It does this by creating a sink from the source it receives and returning the sink
from the RPC handler, causing it to be received as a source by the client. The following
full example shows a server sending a message containing an `sstring` to a client.

Server handler will look like that:

```cpp
    rpc_proto.register_handler(1, [] (int aux_data, rpc::source<int, long> source) {
        rpc::sink<sstring> sink = source.make_sink<serializer, sstring>();
        // use sink and source asynchronously
        return sink;
    });
```

Client code will be:

```cpp
   auto rpc_call = rpc_proto.make_client<rpc::source<sstring> (int, rpc::sink<int, long>)>(1);
   rpc::sink<int, long> sink = rc.make_stream_sink<serializer, int, long>().get();
   rpc::source<sstring> source = rpc_call(rc, aux_data, sink).get();
   // use sink and source here
```

### Closing a stream

Both halves of a stream must be closed by the application:

* **Close the sink** with `rpc::sink::close()` and wait for the returned
  future. Dropping an unclosed sink is an error.
* **Read the source until eof or error.** `rpc::source` deliberately has no
  `close()`: reading it until it yields an unengaged optional (eof) or throws
  is what closes it. If you no longer want the data, tell the peer at the
  application level and then drain what is still in flight.

The stream's connection is shut down only once both halves are closed.

This applies on error too. The calls may fail, and the failures are expected,
but they still have to be made.

## Implementation notes

### RPC stream creation

An RPC stream is implemented as a separate TCP connection. The RPC server knows that a connection
will be used for streaming if during RPC negotiation `Stream parent` feature is present.
The feature will contain ID of an RPC client that was used to create the stream.

So in the example from previous chapter:

```cpp
    rpc::sink<int, long> sink = rc.make_stream_sink<serializer, int, long>().get();
```

the call will initiate a new TCP connection to the same server `rc` is connected to. During RPC
protocol negotiation this connection will have `Stream parent` feature with `rc`'s ID as a value.

### Passing sink/source over RPC call

When `rpc::sink` is sent over an RPC call, it is serialized as its connection ID. The server's RPC handler
then looks up the connection and creates an `rpc::source` from it. When an RPC handler returns `rpc::sink`,
the same happens in the other direction.
