# RPC provided compression infrastructure

## Compression algorithm negotiation

RPC protocol only defines `COMPRESS` feature bit but does not define format of its data.
If application supports multiple compression algorithms it may use the data for algorithm
negotiation. RPC provides convenience class `multi_algo_compressor_factory` to do it
so that each application will not have to re-implement the same logic. The class gets list
of supported compression algorithms and send them as comma separated list in `COMPRESS` feature
payload. On receiving of the list it matches common algorithm between client and server. In case
there is more than one the order of algorithms in clisnt's list is considered to be a tie breaker.

### COMPRESS feature data format when multi_algo_compressor_factory is used

Comma separated string of algorithms names 

