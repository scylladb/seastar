// RESP, the Redis protocol: reading requests and formatting replies.
#pragma once

#include "shoal.hh"
#include <seastar/core/iostream.hh>
#include <seastar/core/temporary_buffer.hh>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <vector>

// snippet: request
// A request: the command's name, followed by its arguments.
using request = std::vector<sstring>;

// A request that breaks the protocol.
class protocol_error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};
// end snippet: request

// snippet: reader
// Reads requests from a client's input stream.
class request_reader {
    input_stream<char>& _in;
    temporary_buffer<char> _buf;    // data read from _in, but not used yet
public:
    explicit request_reader(input_stream<char>& in) : _in(in) {}
    // The next request, or nothing if the client closed the connection.
    future<std::optional<request>> read();
private:
    future<std::optional<sstring>> read_line();
    future<sstring> read_exactly(size_t n);
    future<bool> read_more();
};
// end snippet: reader

// Replies, formatted in RESP.
sstring simple_string_reply(std::string_view s);    // +OK
sstring error_reply(std::string_view message);      // -ERR message
sstring integer_reply(int64_t n);                    // :3
sstring bulk_string_reply(std::string_view s);       // $5 hello
sstring null_reply();                                // $-1
// The start of an array reply of n elements, which must follow it.
sstring array_header_reply(size_t n);                // *3
