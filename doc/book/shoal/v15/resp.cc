// RESP, the Redis protocol: reading requests and formatting replies.
#include "resp.hh"
#include <seastar/core/coroutine.hh>
#include <seastar/coroutine/exception.hh>
#include <fmt/format.h>
#include <algorithm>
#include <cstdlib>
#include <ranges>
#include <string>

// Limits, as in Redis: an inline request's line, a request's number of
// arguments, and an argument's size.
constexpr size_t max_line = 64 * 1024;
constexpr int64_t max_arguments = 1024 * 1024;
constexpr int64_t max_argument_size = 512 * 1024 * 1024;

// snippet: admit
// Requests whose arguments use more memory than this, in all, are admitted
// by the memory semaphore, before the argument that brings them over it is
// allocated. Smaller ones are parsed from the stream's buffers, whose size
// is limited.
constexpr size_t large_request = 64 * 1024;

// Takes n units from the memory semaphore for the current request. The
// first large argument of a request waits for its units. Later ones take
// theirs without waiting, even beyond the limit: a request that waited
// while holding units could wait forever, if the requests holding all the
// other units were waiting too.
future<> request_reader::admit(size_t n) {
    // A single request larger than the limit gets all the units, and waits
    // for all the other requests to give theirs back.
    _admitted = co_await get_units(_memory, std::min(n, _memory_limit));
}

// Takes n more units for the current request, which was admitted already,
// without waiting.
void request_reader::admit_more(size_t n) {
    _admitted.adopt(consume_units(_memory, std::min(n, _memory_limit)));
}
// end snippet: admit

// snippet: read
future<std::optional<request>> request_reader::read() {
    // The previous request was executed: give its memory back, unless its
    // caller took it.
    _admitted.return_all();
    // Usually the last buffer held more requests: return the next one,
    // without a coroutine.
    if (!_parsed.empty()) {
        auto req = std::move(_parsed.front());
        _parsed.pop_front();
        return make_ready_future<std::optional<request>>(std::move(req));
    }
    if (_error) {
        return make_exception_future<std::optional<request>>(_error);
    }
    return read_slow();
}

// Parses what the client sends next, until it ends a request.
future<std::optional<request>> request_reader::read_slow() {
    while (_parsed.empty()) {
        if (_error) {
            co_await coroutine::return_exception_ptr(_error);
        }
        if (_state == state::admit) {
            // The request so far, and the argument that comes next.
            _req_bytes += sizeof(sstring) + _number;
            co_await admit(_req_bytes);
            _req_admitted = true;
            start_argument(_number);
        }
        if (_eof) {
            if (_state == state::start) {
                co_return std::nullopt;
            }
            co_await coroutine::return_exception(
                protocol_error("unexpected end of input"));
        }
        co_await _in.consume([this] (temporary_buffer<char> buf) {
            // parse_buffer() may still throw, std::bad_alloc for example:
            // return it as a failed future.
            return futurize_invoke([&] {
                return parse_buffer(std::move(buf));
            });
        });
    }
    auto req = std::move(_parsed.front());
    _parsed.pop_front();
    co_return std::move(req);
}
// end snippet: read

// snippet: parse-buffer
// Parses one of the stream's buffers, as input_stream::consume() asks of a
// consumer: continue_consuming asks for the next buffer, and
// stop_consuming returns what wasn't parsed yet to the stream, which gives
// it to the consumer again the next time.
consumption_result<char> request_reader::parse_buffer(
        temporary_buffer<char> buf) {
    if (buf.empty()) {
        _eof = true;    // the client closed its side
        return continue_consuming();
    }
    _buf_begin = buf.begin();
    const char* stop;
    try {
        stop = parse(buf.begin(), buf.end());
    } catch (protocol_error&) {
        // Keep the error until the requests before it have been read.
        _error = std::current_exception();
        return stop_consuming(temporary_buffer<char>());
    }
    _buf_offset += stop - buf.begin();
    if (_state == state::admit || !_parsed.empty()) {
        // Return the requests, or admit a large argument before going on.
        buf.trim_front(stop - buf.begin());
        return stop_consuming(std::move(buf));
    }
    return continue_consuming();
}
// end snippet: parse-buffer

// snippet: parse
// Parses the characters from p to end, adding the requests they finish
// to _parsed. Returns where it stopped: at end, or after the "$5" line of
// a large argument, which must be admitted first.
const char* request_reader::parse(const char* p, const char* end) {
    while (p != end) {
        switch (_state) {
        case state::start:
            if (*p == 0) {
                // The zeros that pad a log's blocks (see log.cc), between
                // its requests; a client never sends them.
                p++;
            } else if (*p == '*') {
                _state = state::count;
                _number = _digits = 0;
                _cr = false;
                p++;
            } else {
                // An "inline" request, typed into nc: words separated by
                // spaces.
                _state = state::inline_request;
            }
            break;
        // snippet: count-length
        case state::count:
            if (parse_number(*p++, max_arguments, "invalid multibulk length")) {
                if (_number == 0) {
                    throw protocol_error("invalid multibulk length");
                }
                // Don't reserve room for all the arguments: they haven't
                // arrived, and may never arrive. The vector grows as they
                // do, and its memory is counted with theirs.
                _left = _number;
                _state = state::dollar;
            }
            break;
        case state::dollar:
            if (*p++ != '$') {
                throw protocol_error("expected '$'");
            }
            _state = state::length;
            _number = _digits = 0;
            _cr = false;
            break;
        case state::length:
            if (parse_number(*p++, max_argument_size, "invalid bulk length")) {
                // The argument's memory, with its place in the vector.
                size_t size = sizeof(sstring) + _number;
                if (_req_admitted) {
                    admit_more(size);
                } else if (_req_bytes + size > large_request) {
                    _state = state::admit;
                    return p;
                }
                _req_bytes += size;
                start_argument(_number);
            }
            break;
        // end snippet: count-length
        case state::argument: {
            // Copy as much of the argument, and of the \r\n after it, as
            // this buffer has.
            auto& arg = _req.back();
            size_t n = std::min(size_t(end - p), arg.size() - _filled);
            std::copy_n(p, n, arg.begin() + _filled);
            _filled += n;
            p += n;
            if (_filled == arg.size()) {
                if (!arg.ends_with("\r\n")) {
                    throw protocol_error("expected end of line");
                }
                arg.resize(arg.size() - 2);
                end_argument();
                if (_state == state::start) {
                    _parsed_end = _buf_offset + (p - _buf_begin);
                }
            }
            break;
        }
        case state::inline_request: {
            auto newline = std::find(p, end, '\n');
            _line.append(p, newline);
            if (_line.size() > max_line) {
                throw protocol_error("line too long");
            }
            p = newline;
            if (p != end) {
                p++;
                end_inline_request();
                _parsed_end = _buf_offset + (p - _buf_begin);
            }
            break;
        }
        case state::admit:
            // read_slow() admits the argument before parsing goes on.
            std::abort();
        }
    }
    return p;
}
// end snippet: parse

// snippet: parse-number
// Parses one character of a "*3" or "$5" line, after its first character.
// Returns true at the line's end. The number must be between 0 and max.
bool request_reader::parse_number(char c, int64_t max, const char* error) {
    if (c >= '0' && c <= '9' && !_cr) {
        _number = _number * 10 + (c - '0');
        _digits++;
        if (_number > max) {
            throw protocol_error(error);
        }
        return false;
    }
    if (c == '\r' && !_cr && _digits > 0) {
        _cr = true;
        return false;
    }
    if (c == '\n' && _digits > 0) {
        return true;
    }
    throw protocol_error(error);
}
// end snippet: parse-number

// snippet: arguments
// Starts an argument of the given length, which the next characters fill,
// with the \r\n after it.
void request_reader::start_argument(size_t length) {
    _req.emplace_back(sstring::initialized_later(), length + 2);
    _filled = 0;
    _state = state::argument;
}

// Ends an argument, and the request, if it was the last.
void request_reader::end_argument() {
    if (--_left > 0) {
        _state = state::dollar;
        return;
    }
    _parsed.push_back(std::move(_req));
    _req = {};
    _req_bytes = 0;
    _req_admitted = false;
    _state = state::start;
}

// Ends an inline request, at its '\n'.
void request_reader::end_inline_request() {
    std::string_view line = _line;
    if (line.ends_with('\r')) {
        line.remove_suffix(1);
    }
    request req;
    for (auto word : line | std::views::split(' ')) {
        if (!word.empty()) {
            req.emplace_back(std::string_view(word));
        }
    }
    // An empty line is no request: wait for the next.
    if (!req.empty()) {
        _parsed.push_back(std::move(req));
    }
    _line.clear();
    _state = state::start;
}
// end snippet: arguments

// snippet: replies
sstring simple_string_reply(std::string_view s) {
    return fmt::format("+{}\r\n", s);
}

sstring error_reply(std::string_view message) {
    return fmt::format("-ERR {}\r\n", message);
}

sstring integer_reply(int64_t n) {
    return fmt::format(":{}\r\n", n);
}

sstring bulk_string_reply(std::string_view s) {
    return fmt::format("${}\r\n{}\r\n", s.size(), s);
}

sstring null_reply() {
    return "$-1\r\n";
}
// end snippet: replies

sstring null_array_reply() {
    return "*-1\r\n";
}

sstring wrong_type_reply() {
    return "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n";
}

sstring array_header_reply(size_t n) {
    return fmt::format("*{}\r\n", n);
}

// snippet: reply-builder-def
void reply_builder::append(std::string_view s) {
    while (!s.empty()) {
        if (_pieces.empty() || _used == piece_size) {
            _pieces.emplace_back(piece_size);
            _used = 0;
        }
        size_t n = std::min(s.size(), piece_size - _used);
        std::ranges::copy(s.substr(0, n), _pieces.back().get_write() + _used);
        _used += n;
        s.remove_prefix(n);
    }
}

void reply_builder::append_bulk_string(std::string_view s) {
    char header[32];
    auto end = fmt::format_to(header, "${}\r\n", s.size());
    append(std::string_view(header, end));
    append(s);
    append("\r\n");
}

std::vector<temporary_buffer<char>> reply_builder::finish() && {
    if (!_pieces.empty()) {
        _pieces.back().trim(_used);
    }
    return std::move(_pieces);
}

future<> write_reply(output_stream<char>& out, reply r) {
    if (auto* s = std::get_if<sstring>(&r)) {
        co_await out.write(*s);
        co_return;
    }
    for (auto& piece : std::get<1>(r)) {
        co_await out.write(std::move(piece));
    }
}
// end snippet: reply-builder-def
