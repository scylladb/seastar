# RPC protocol

## Data encoding

All integral data is encoded in little endian format.

## Protocol negotiation

The negotiation works by exchanging negotiation frame immediately after connection establishment. The negotiation frame format is:

    uint8_t magic[8] = SSTARRPC
    uint32_t len
    uint8_t data[len]

The negotiation frame data is itself composed of multiple records, one for each feature number present.  Feature numbers begin at zero and will be defined by later versions of this document.


     struct negotiation_frame_feature_record {
         uint32_t feature_number;
         uint32_t len;
         uint8_t data[len];
     }

A `negotiation_frame_feature_record` signals that an optional feature is present in the client, and can contain additional feature-specific data.  The feature number will be omitted in a server response if an optional feature is declined by the server.

Actual negotiation looks like this:

         Client                                  Server
    --------------------------------------------------------------------------------------------------
    send negotiation frame
                                        recv frame
                                        check magic (disconnect if magic is not SSTARRPC)
                                        send negotiation frame back
    recv frame
    check magic (disconnect if magic is not SSTARRPC)

### Supported features

#### Compression
    feature_number:  0
    data          :  opaque data that is passed to a compressor factory
                     provided by an application. Compressor factory is
                     responsible for negotiation of compression algorithm.

    If compression is negotiated request and response frames are encapsulated in a compressed frame.

#### Timeout propagation
    feature_number:  1
    data          :  none

    If timeout propagation is negotiated request frame has additional 8 bytes that hold timeout value
    for a request in milliseconds. Zero value means that timeout value was not specified.
    If timeout is specified and server cannot handle the request in specified time frame it my choose
    to not send the reply back (sending it back will not be an error either).

#### Connection ID
    feature_number: 2
    uint64_t conenction_id  : RPC connection ID

    Server assigns unique connection ID for each connection and sends it to a client using
    this feature.

#### Stream parent
    feature_number: 3
    uint64_t connection_id : RPC connection ID representing a parent of the stream

    If this feature is present it means that the connection is not regular RPC connection
    but stream connection. If parent connection is closed or aborted all streams belonging
    to it will be closed as well.

    Stream connection is a connection that allows bidirectional flow of bytes which may carry one or
    more messages in each direction. Stream connection should be explicitly closed by both client and
    server. Closing is done by sending special EOS frame (described below).


#### Isolation
    feature number: 4
    uint32_t isolation_cookie_len
    uint8_t isolation_cookie[len]

    The `isolation_cookie` field is used by the server to select a
    `seastar::scheduling_group` (or equivalent in another implementation) that
    will run this connection. In the future it will also be used for rpc buffer
    isolation, to avoid rpc traffic in one isolation group from starving another.

    The server does not directly assign meaning to values of `isolation_cookie`;
    instead, the interpretation is left to user code.

#### Handler duration
    feature number: 5
    data: none

    Asks server to send "extended" response that includes the handler duration time. See
    the response frame description for more details


##### Compressed frame format
    uint32_t len
    uint8_t compressed_data[len]

    After compressed_data is uncompressed, it becomes a regular request, response or streaming frame.

    As a special case, it is allowed to send a compressed frame of size 0 (pre-compression).
    Such a frame will be a no-op on the receiver.
    (This can be used as a means of communication between the compressors themselves.
    If a compressor wants to send some metadata to its peer, it can send a no-op frame,
    and prepend the metadata as a compressor-specific header.)

## Request frame format
    uint64_t timeout_in_ms - only present if timeout propagation is negotiated
    uint64_t verb_type
    int64_t msg_id
    uint32_t len
    uint8_t data[len]

msg_id has to be positive and may never be reused.
data is transparent for the protocol and serialized/deserialized by a user

## Response frame format
    int64_t msg_id
    uint32_t len
    uint32_t handler_duration - present if handler duration is negotiated
    uint8_t data[len]

if msg_id < 0 enclosed response contains an exception that came as a response to msg id abs(msg_id)
data is transparent for the protocol and serialized/deserialized by a user

the handler_duration is in microseconds, the value of 0xffffffff means that it wasn't measured
and should be disregarded by client

## Stream frame format
   uint32_t len
   uint8_t data[len]

len == 0xffffffff signals end of stream
data is transparent for the protocol and serialized/deserialized by a user

## Exception encoding
    uint32_t type
    uint32_t len
    uint8_t data[len]

### Known exception types
    USER = 0
    UNKNOWN_VERB = 1

#### USER exception encoding

    uint32_t len
    char[len]

This exception is sent as a reply if rpc handler throws an exception.
It is delivered to a caller as rpc::remote_verb_error(char[len])

#### UNKNOWN_VERB exception encoding

    uint64_t verb_id

This exception is sent as a response to a request with unknown verb_id, the verb id is passed back as part of the exception payload.

## More formal protocol description

	request_stream = negotiation_frame, { request | compressed_request }
	request = verb_type, msg_id, len, { byte }*len
	compressed_request = len, { bytes }*len
	response_stream = negotiation_frame, { response | compressed_response }
	response = reply | exception
	compressed_response = len, { byte }*len
        streaming_stream = negotiation_frame, { streaming_frame | compressed_streaming_frame }
        streaming_frame = len, { byte }*len
        compressed_streaming_frame = len, { byte }*len
	reply = msg_id, len, { byte }*len
	exception = exception_header, serialized_exception
	exception_header = -msg_id, len
	serialized_exception = (user|unknown_verb)
	user = len, {byte}*len
	unknown_verb = verb_type
	verb_type = uint64_t
	msg_id = int64_t
	len = uint32_t
	byte = uint8_t
	negotiation_frame = 'SSTARRPC' len32(negotiation_frame_data) negotiation_frame_data
	negotiation_frame_data = negotiation_frame_feature_record*
	negotiation_frame_feature_record = feature_number len {byte}*len
	feature_number = uint32_t

Note that replies can come in order different from requests, and some requests may not have a reply at all.

