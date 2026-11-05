// RESP, the Redis protocol: reading requests and formatting replies.
#include "resp.hh"
#include <seastar/core/coroutine.hh>
#include <seastar/coroutine/exception.hh>
#include <fmt/format.h>
#include <algorithm>
#include <charconv>
#include <ranges>
#include <string>

// Limits, as in Redis: a request's first line, a request's number of
// arguments, and an argument's size.
constexpr size_t max_line = 64 * 1024;
constexpr int64_t max_arguments = 1024 * 1024;
constexpr int64_t max_argument_size = 512 * 1024 * 1024;

// snippet: read-more
// Adds to _buf whatever the client sent next. Returns false if the client
// closed the connection.
future<bool> request_reader::read_more() {
    auto more = co_await _in.read();
    if (more.empty()) {
        co_return false;
    }
    if (_buf.empty()) {
        _buf = std::move(more);
    } else {
        temporary_buffer<char> joined(_buf.size() + more.size());
        std::ranges::copy(_buf, joined.get_write());
        std::ranges::copy(more, joined.get_write() + _buf.size());
        _buf = std::move(joined);
    }
    co_return true;
}
// end snippet: read-more

// snippet: read-line
// Reads a line, without its line ending. Returns nothing if the client
// closed the connection before a line began. Usually the line is already
// in _buf, so this isn't a coroutine: it returns an available future, and
// leaves the waiting to the coroutine read_line_slow().
future<std::optional<sstring>> request_reader::read_line() {
    auto newline = std::ranges::find(_buf, '\n');
    if (newline == _buf.end()) {
        return read_line_slow();
    }
    return make_ready_future<std::optional<sstring>>(
        take_line(newline - _buf.begin()));
}
// end snippet: read-line

// snippet: take-line
// Removes from _buf a line that ends with '\n' at _buf[length], and returns
// it without its line ending.
sstring request_reader::take_line(size_t length) {
    bool has_cr = length > 0 && _buf[length - 1] == '\r';
    sstring line(_buf.get(), has_cr ? length - 1 : length);
    _buf.trim_front(length + 1);
    return line;
}
// end snippet: take-line

// snippet: read-line-slow
future<std::optional<sstring>> request_reader::read_line_slow() {
    while (true) {
        auto newline = std::ranges::find(_buf, '\n');
        if (newline != _buf.end()) {
            co_return take_line(newline - _buf.begin());
        }
        if (_buf.size() > max_line) {
            co_await coroutine::return_exception(
                protocol_error("line too long"));
        }
        if (!co_await read_more()) {
            if (_buf.empty()) {
                co_return std::nullopt;
            }
            co_await coroutine::return_exception(
                protocol_error("unexpected end of input"));
        }
    }
}
// end snippet: read-line-slow

// snippet: read-exactly
// Reads exactly n bytes, without a coroutine if _buf has them.
future<sstring> request_reader::read_exactly(size_t n) {
    if (_buf.size() < n) {
        return read_exactly_slow(n);
    }
    sstring s(_buf.get(), n);
    _buf.trim_front(n);
    return make_ready_future<sstring>(std::move(s));
}
// end snippet: read-exactly

// Reads exactly n bytes, more than _buf has. Long arguments are read
// directly from the stream, without copying them through _buf.
future<sstring> request_reader::read_exactly_slow(size_t n) {
    sstring s(sstring::initialized_later(), n);
    size_t from_buf = _buf.size();
    std::copy_n(_buf.begin(), from_buf, s.begin());
    _buf.trim_front(from_buf);
    auto rest = co_await _in.read_exactly(n - from_buf);
    if (rest.size() < n - from_buf) {
        co_await coroutine::return_exception(
            protocol_error("unexpected end of input"));
    }
    std::ranges::copy(rest, s.begin() + from_buf);
    co_return s;
}

// Parses the number after the first character of a line such as "*3" or
// "$5", or returns nothing if it isn't a number between 0 and max.
static std::optional<int64_t> parse_number(const sstring& line, int64_t max) {
    int64_t n;
    const char* line_end = line.data() + line.size();
    auto [end, error] = std::from_chars(line.data() + 1, line_end, n);
    if (error != std::errc() || end != line_end || n < 0 || n > max) {
        return std::nullopt;
    }
    return n;
}

// snippet: admit
// A request whose arguments need more memory than this, in all, is
// admitted by the memory semaphore before the argument that brings it over
// the limit is read. Smaller requests are read through _buf, whose size is
// limited.
constexpr size_t large_request = 64 * 1024;

// Takes n units from the memory semaphore for the current request. The
// request's first admission waits for its units. Later ones take theirs
// without waiting, even beyond the limit: a request that waited while
// holding units could wait forever, if the requests holding all the other
// units were waiting too.
future<> request_reader::admit(size_t n) {
    // A single request larger than the limit gets all the units, and waits
    // for all the other requests to give theirs back.
    n = std::min(n, _memory_limit);
    if (_admitted) {
        _admitted.adopt(consume_units(_memory, n));
    } else {
        _admitted = co_await get_units(_memory, n);
    }
}
// end snippet: admit

// snippet: read
future<std::optional<request>> request_reader::read() {
    // The previous request was executed: give its memory back.
    _admitted.return_all();
    while (true) {
        auto line = co_await read_line();
        if (!line) {
            co_return std::nullopt;
        }
        if (!line->starts_with('*')) {
            // An "inline" request, typed into nc: words separated by spaces.
            request req;
            auto words = std::string_view(*line) | std::views::split(' ');
            for (auto word : words) {
                if (!word.empty()) {
                    req.emplace_back(std::string_view(word));
                }
            }
            if (req.empty()) {
                continue;   // an empty line: wait for the next request
            }
            co_return req;
        }
        // An array of "bulk strings": *<count>, then <count> times
        // $<length> followed by the string itself.
        auto count = parse_number(*line, max_arguments);
        if (!count || *count == 0) {
            co_await coroutine::return_exception(
                protocol_error("invalid multibulk length"));
        }
        // Don't reserve room for all the arguments: a client can announce
        // a million, and send none. The memory of those that come is
        // counted, each with its place in the request.
        request req;
        size_t memory = 0;
        for (int64_t i = 0; i < *count; i++) {
            auto header = co_await read_line();
            if (!header || !header->starts_with('$')) {
                co_await coroutine::return_exception(
                    protocol_error("expected '$'"));
            }
            auto length = parse_number(*header, max_argument_size);
            if (!length) {
                co_await coroutine::return_exception(
                    protocol_error("invalid bulk length"));
            }
            size_t size = sizeof(sstring) + *length;
            memory += size;
            if (memory > large_request) {
                // The first time, the whole request so far; then each
                // argument that follows.
                co_await admit(_admitted ? size : memory);
            }
            // The string, followed by its \r\n.
            auto arg = co_await read_exactly(*length + 2);
            if (!arg.ends_with("\r\n")) {
                co_await coroutine::return_exception(
                    protocol_error("expected end of line"));
            }
            arg.resize(*length);
            req.push_back(std::move(arg));
        }
        co_return req;
    }
}
// end snippet: read

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
