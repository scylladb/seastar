# RPC provided compression infrastructure

## Compression algorithm negotiation

The RPC protocol defines the `COMPRESS` feature bit but not the format of its data.
If an application supports multiple compression algorithms, it may use this data for
negotiation. RPC provides the `multi_algo_compressor_factory` convenience class so that
applications do not have to reimplement the same logic. The class accepts a list of supported
compression algorithms and sends them as a comma-separated list in the client's `COMPRESS`
feature payload. On receiving the list, it finds algorithms supported by both the client and server.
If there is more than one, their order in the client's list acts as a tiebreaker (the first algorithm
wins). Once the server chooses a compressor, it puts that compressor's identifier in the returned
`COMPRESS` feature payload, informing the client which algorithm to use
for the connection.

## Compression algorithms

### `LZ4` compressor

This compressor uses LZ4 to compress and decompress RPC messages. It requires all memory buffers to be contiguous, which may force it to temporarily linearise fragmented messages. LZ4 is fast enough to often make the cost of those copies not negligible compared to the cost of the whole compression or decompression routine. Therefore, this algorithm is best suited if there is an upper bound of the message size and they are expected to fit in a single fragment of input and output memory buffers.

### `LZ4_FRAGMENTED` compressor

This compressor uses LZ4 streaming interface to compress and decompress even large messages without linearising them. The LZ4 streaming routines tend to be slower than the basic ones and the general logic for handling buffers is more complex, so this compressor is best suited only when there is no clear upper bound on the message size or if the messages are expected to be fragmented.

Internally, the compressor processes data in 32 kB chunks and tries to avoid unnecessary copies. Applications should therefore use memory-buffer fragment sizes that are an integral multiple of 32 kB.
