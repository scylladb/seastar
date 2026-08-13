# RPC provided compression infrastructure

## Compression algorithm negotiation

RPC protocol only defines `COMPRESS` feature bit but does not define format of its data.
If application supports multiple compression algorithms it may use the data for algorithm
negotiation. RPC provides convenience class `multi_algo_compressor_factory` to do it
so that each application will not have to re-implement the same logic. The class gets list
of supported compression algorithms and send them as comma separated list in the client `COMPRESS`
feature payload. On receiving of the list it matches common algorithm between client and server.
In case there is more than one the order of algorithms in client's list is considered to be a tie
breaker (first algorithm wins). Once a compressor is chosen by the server, it puts the identifier of
this in the returned `COMPRESS` feature payload, informing the client of which algorithm should be used
for the connection.

## Compression algorithms

### `LZ4` compressor

This compressor uses LZ4 to compress and decompress RPC messages. It requires all memory buffers to be contiguous, which may force it to temporarily linearise fragmented messages. LZ4 is fast enough to often make the cost of those copies not negligible compared to the cost of the whole compression or decompression routine. Therefore, this algorithm is best suited if there is an upper bound of the message size and they are expected to fit in a single fragment of input and output memory buffers.

### `LZ4_FRAGMENTED` compressor

This compressor uses LZ4 streaming interface to compress and decompress even large messages without linearising them. The LZ4 streaming routines tend to be slower than the basic ones and the general logic for handling buffers is more complex, so this compressor is best suited only when there is no clear upper bound on the message size or if the messages are expected to be fragmented.

Internally, the compressor processes data in a 32 kB chunks and tries to avoid unnecessary copies as much as possible. It is therefore, recommended, that the application uses memory buffer fragment sizes that are an integral multiple of 32 kB.

## Opportunistic frame batching

When compression is enabled, RPC will opportunistically coalesce several messages that
are already sitting in the outgoing queue into a single compressed blob, instead of
compressing and flushing each one individually. This only kicks in for messages that are
already queued behind one another at the moment the front one is about to be sent -- a
lone message with nothing else queued is sent exactly as it would be without this
feature, with no added latency.

This is gated by the `BATCH_FRAMES` protocol feature, which each end advertises during
negotiation to declare that its receive loop can demultiplex more than one frame out of
a single decompressed blob. A sender only batches once the peer has confirmed support;
against an older peer that predates this feature (and therefore never advertises it),
every message is still sent as its own compressed blob, exactly as before. This behaviour
can also be disabled explicitly via `client_options::batch_outgoing_frames` /
`server_options::batch_outgoing_frames`.

The receiving side of the demultiplexing logic is unconditional and always active: a
decompressed blob is read until exhausted before a new one is fetched off the wire. This
is safe regardless of whether the peer actually batches, since a blob holding a single
frame (the case when talking to an older peer, or when nothing else happened to be
queued) is simply found exhausted right after that one frame is parsed.

The number of messages, and total bytes, that may be coalesced into one blob are bounded
by `connection::max_batched_messages` and `connection::max_batched_bytes` in
`rpc.hh`.
