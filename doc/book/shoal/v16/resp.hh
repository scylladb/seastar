// RESP, the Redis protocol: reading requests and formatting replies.
#pragma once

#include "shoal.hh"
#include <seastar/core/iostream.hh>
#include <seastar/core/semaphore.hh>
#include <seastar/core/temporary_buffer.hh>
#include <cstdint>
#include <deque>
#include <exception>
#include <fmt/format.h>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
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
// Reads requests from a client's input stream. It parses each of the
// stream's buffers, as it comes, into the requests it holds; the end of a
// buffer usually cuts a request in two, and the parser keeps its place
// in it, so it never needs to join one buffer to the next. The memory of
// large requests is admitted by a semaphore, shared by all the
// connections, before it's allocated, and the units are kept until the
// next read(), which comes after the request is executed.
class request_reader {
    input_stream<char>& _in;
    semaphore& _memory;             // bytes of large arguments, in all requests
    size_t _memory_limit;           // the semaphore's units, in all
    semaphore_units<> _admitted;    // the current request's units
    std::deque<request> _parsed;    // whole requests, not yet read
    // Where the parser is in the request it's parsing.
    enum class state {
        start,          // before a request
        count,          // in the "*3" line of an array of 3 arguments
        dollar,         // before the '$' of an argument's "$5" line
        length,         // in an argument's "$5" line, after the '$'
        admit,          // after the "$5" line of a large argument
        argument,       // in the argument, or its \r\n
        inline_request, // in an "inline" request: words and spaces
    };
    state _state = state::start;
    request _req;                   // the request's arguments so far
    int64_t _number;                // the "*3" or "$5" being parsed
    int _digits;                    // its digits so far
    bool _cr;                       // whether its \r was seen
    int64_t _left;                  // the arguments not parsed yet
    size_t _filled;                 // how much of the argument was copied
    size_t _req_bytes = 0;          // the request's memory so far
    bool _req_admitted = false;     // whether the request has units
    std::string _line;              // the inline request so far
    std::exception_ptr _error;      // a protocol error after _parsed
    const char* _buf_begin;         // the buffer being parsed,
    uint64_t _buf_offset = 0;       // and its offset in the stream
    uint64_t _parsed_end = 0;       // the offset after the last request
    bool _eof = false;              // whether the client closed its side
public:
    request_reader(input_stream<char>& in, semaphore& memory, size_t memory_limit)
        : _in(in), _memory(memory), _memory_limit(memory_limit) {}
    // The next request, or nothing if the client closed the connection.
    future<std::optional<request>> read();
    // Takes the units of the request just read, which otherwise read()
    // gives back when it reads the next request: a request executed while
    // the next ones are read keeps its units until it's done.
    semaphore_units<> take_units() { return std::move(_admitted); }
    // The requests already parsed, that read() will return next.
    const std::deque<request>& parsed() const { return _parsed; }
    // The offset in the stream just after the last whole request.
    uint64_t parsed_end() const { return _parsed_end; }
private:
    future<std::optional<request>> read_slow();
    consumption_result<char> parse_buffer(temporary_buffer<char> buf);
    const char* parse(const char* p, const char* end);
    bool parse_number(char c, int64_t max, const char* error);
    void start_argument(size_t length);
    void end_argument();
    void end_inline_request();
    future<> admit(size_t n);
    void admit_more(size_t n);
};
// end snippet: reader

// snippet: reply-builder
// A reply that may be large, built in pieces of 16 KB, as Valkey builds
// its replies: it needs no large contiguous memory, isn't copied as it
// grows, and is written to the stream without being copied again.
class reply_builder {
    static constexpr size_t piece_size = 16 * 1024;
    std::vector<temporary_buffer<char>> _pieces;    // the last may have room
    size_t _used = 0;                               // bytes used in the last
public:
    void append(std::string_view s);
    // Appends s as a bulk string: $5 hello.
    void append_bulk_string(std::string_view s);
    // The reply, in pieces, without the last one's unused room.
    std::vector<temporary_buffer<char>> finish() &&;
};

// A command's reply: usually one string, but a large one may come in
// pieces.
using reply = std::variant<sstring, std::vector<temporary_buffer<char>>>;

// Writes a reply to the stream. Pieces are written without copying them,
// so the stream must be flushed before anything else is written to it.
future<> write_reply(output_stream<char>& out, reply r);
// end snippet: reply-builder

// snippet: format-request
// Formats a request, made of these words, in RESP, and gives each of its
// pieces, in turn, to copy(), which copies it where it belongs: into the
// log, or, since version 14, into the replication feed. The "*3" and "$5"
// lines are formatted on the stack, without allocating a string, which
// would cost more than the copying.
template <typename Words, typename Copy>
void format_request(const Words& words, Copy&& copy) {
    char line[24];
    auto copy_line = [&] (char c, size_t n) {
        auto end = fmt::format_to(line, "{}{}\r\n", c, n);
        copy(std::string_view(line, end));
    };
    copy_line('*', std::size(words));
    for (std::string_view word : words) {
        copy_line('$', word.size());
        copy(word);
        copy("\r\n");
    }
}
// end snippet: format-request

// Replies, formatted in RESP.
sstring simple_string_reply(std::string_view s);    // +OK
sstring error_reply(std::string_view message);      // -ERR message
sstring integer_reply(int64_t n);                    // :3
sstring bulk_string_reply(std::string_view s);       // $5 hello
sstring null_reply();                                // $-1
sstring null_array_reply();                          // *-1
// The error of a command applied to a key of the wrong type.
sstring wrong_type_reply();
// The start of an array reply of n elements, which must follow it.
sstring array_header_reply(size_t n);                // *3
