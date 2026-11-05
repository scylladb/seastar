// A shard's append-only log: every change to the shard's data, as the
// request that made it, in RESP, as Redis's AOF keeps them. When Shoal
// starts, it replays the log to rebuild the data.
#pragma once

#include "resp.hh"
#include <seastar/core/file.hh>
#include <seastar/core/semaphore.hh>
#include <seastar/core/temporary_buffer.hh>
#include <cstdint>
#include <deque>
#include <fmt/format.h>
#include <iterator>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string_view>

// snippet: log
class append_only_log {
    file _file;
    // The records that aren't in the file yet, in aligned chunks; the first
    // chunk begins at file offset _tail_start.
    std::deque<temporary_buffer<char>> _tail;
    uint64_t _tail_start = 0;
    uint64_t _size = 0;             // the log's bytes, in all
    uint64_t _written = 0;          // of them, those in the file
    uint64_t _appends = 0;          // requests appended since it was opened
    semaphore _committing{1};       // one commit at a time
public:
    // Opens the log in the file path, creating it if it's new, and calls
    // replay() with each of the requests it holds, in order.
    static future<std::unique_ptr<append_only_log>> open(
        sstring path, std::function<void(const request&)> replay);
    // Appends a request, made of these words, in RESP. It isn't on the
    // disk until a commit.
    template <typename Words>
    void append(const Words& words) {
        copy_line('*', std::size(words));
        for (std::string_view word : words) {
            copy_line('$', word.size());
            copy(word);
            copy("\r\n");
        }
        _appends++;
    }
    // Writes the requests appended so far to the disk, and waits until the
    // disk has them. Commits that overlap share one write.
    future<> commit();
    uint64_t uncommitted() const { return _size - _written; }
    uint64_t appends() const { return _appends; }
    // Commits what's left, and closes the file.
    future<> close();
private:
    void copy(std::string_view s);
    // Copies a line such as "*3\r\n", formatted on the stack, without
    // allocating a string, which would cost more than the copying.
    void copy_line(char c, size_t n) {
        char line[24];
        auto end = fmt::format_to(line, "{}{}\r\n", c, n);
        copy(std::string_view(line, end));
    }
    future<> write_tail();
};
// end snippet: log

// Checks that dir's logs were written by as many shards as Shoal has now,
// since a key's shard depends on their number; for a new dir, records it.
future<> check_shard_count(sstring dir);
