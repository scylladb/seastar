# RPC streaming

## Streaming API

### Sink and Source

Basic element of streaming API is `rpc::sink` and `rpc::source`. The former
is used to send data and the later is to receive it. Client and server
has their own pair of sink and source. `rpc::sink` and `rpc::source` are
templated classes where template parameters describe a type of the data
that is sent/received. For instance the sink that is used to send messages
containing `int` and `long` will be of a type `rpc::sink<int, long>`.  The
opposite end of the stream will have a source of the type `rpc::source<int, long>`
which will be used to receive those messages. Messages are received at a
source as `std::optional` containing an actual message as an `std::tuple`. Unengaged
optional means EOS (end of stream) - the stream was closed by a peer. If
error happen before EOS is received a receiver cannot be sure it received all
the data.

To send the data using `rpc::source<int, long>` one can write (assuming `seastar::async` context):

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
          std:optional<std::tuple<int, long>> data = source().get();
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

To open an RPC stream one needs RPC client to be created already. The stream
will be associated with the client and will be aborted if the client is closed
before streaming is. Given RPC client `rc`, and a `serializer` class that models the Serializer concept (as explained in the rpc::protocol class), one creates `rpc::sink` as follows
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
side it will be used for receive. Now all is left to do is for the client to
invoke this RPC handler with aux_data and the sink.

But what about communicating in another direction: from a server to a
client. For that a server also has to have a sink and a client has to have
a source and since messages in this direction may be of a different type
than from client to server the sink and the source may be of a different
type as well.

Server initiates creation of a communication channel in another direction.
It does this by creating a sink from the source it receives and returning the sink
from RPC handler which will cause it to be received as a source by a client. Lets look
at the full example where server want to send message containing sstring to a client.

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
   auto rpc_call = rpc_proto.make_client<rpc::source<sstring> (int, rpc::sink<int>)>(1);
   rpc::sink<int, long> sink = rc.make_stream_sink<serializer, int, long>().get();
   rpc::source<sstring> source = rpc_call(rc, aux_data, sink).get();
   // use sink and source here
```

## Implementation notes

### RPC stream creation

RPC stream is implemented as a separate TCP connection. RPC server knows that a connection
will be used for streaming if during RPC negotiation `Stream parent` feature is present.
The feature will contain ID of an RPC client that was used to create the stream.

So in the example from previous chapter:

```cpp
    rpc::sink<int, long> sink = rc.make_stream_sink<serializer, int, long>().get();
```

the call will initiate a new TCP connection to the same server `rc` is connected to. During RPC
protocol negotiation this connection will have `Stream parent` feature with `rc`'s ID as a value.

### Passing sink/source over RPC call

When `rpc::sink` is sent over RPC call it is serialized as its connection ID. Server's RPC handler
then lookups the connection and creates an `rpc::source` from it. When RPC handler returns `rpc::sink`
the same happens in other direction.
