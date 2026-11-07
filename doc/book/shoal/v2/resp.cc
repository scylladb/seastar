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
// closed the connection before a line began.
future<std::optional<sstring>> request_reader::read_line() {
    while (true) {
        auto newline = std::ranges::find(_buf, '\n');
        if (newline != _buf.end()) {
            size_t length = newline - _buf.begin();
            bool has_cr = length > 0 && _buf[length - 1] == '\r';
            sstring line(_buf.get(), has_cr ? length - 1 : length);
            _buf.trim_front(length + 1);
            co_return line;
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
// end snippet: read-line

// Reads exactly n bytes. Long arguments are read directly from the stream,
// without copying them through _buf.
future<sstring> request_reader::read_exactly(size_t n) {
    sstring s(sstring::initialized_later(), n);
    size_t from_buf = std::min(n, _buf.size());
    std::copy_n(_buf.begin(), from_buf, s.begin());
    _buf.trim_front(from_buf);
    if (from_buf < n) {
        auto rest = co_await _in.read_exactly(n - from_buf);
        if (rest.size() < n - from_buf) {
            co_await coroutine::return_exception(
                protocol_error("unexpected end of input"));
        }
        std::ranges::copy(rest, s.begin() + from_buf);
    }
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

// snippet: read
future<std::optional<request>> request_reader::read() {
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
        request req;
        req.reserve(*count);
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

sstring array_header_reply(size_t n) {
    return fmt::format("*{}\r\n", n);
}
