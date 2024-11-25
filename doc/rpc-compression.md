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
