// A shard's append-only log: every change to the shard's data, as the
// request that made it, in RESP, as Redis's AOF keeps them. When Shoal
// starts, it replays the log to rebuild the data.
#pragma once

#include "latency.hh"
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

// snippet: log-stats
// What the shard's logs did, for the metrics of version 15: the store
// keeps it, since a rewrite replaces the log, and each of its logs adds to
// it.
struct log_stats {
    uint64_t bytes_written = 0;
    uint64_t flushes = 0;
    latency_histogram flush_latency;
};
// end snippet: log-stats

// snippet: log
class append_only_log {
    file _file;
    // The records that aren't in the file yet, in aligned chunks; the first
    // chunk begins at file offset _tail_start.
    std::deque<temporary_buffer<char>> _tail;
    uint64_t _tail_start = 0;
    uint64_t _size = 0;             // the log's bytes, in all
    uint64_t _written = 0;          // of them, those in the file
    uint64_t _durable = 0;          // and those that the disk has for sure
    uint64_t _appends = 0;          // requests appended since it was opened
    semaphore _committing{1};       // one commit at a time
    // After a rewrite, the log that replaced this one, to which this
    // one's commits go (see hand_over()).
    append_only_log* _successor = nullptr;
public:
    // Where the log counts what it writes and flushes, if anywhere.
    log_stats* stats = nullptr;
    // Opens the log in the file path, creating it if it's new, and calls
    // replay() with each of the requests it holds, in order.
    static future<std::unique_ptr<append_only_log>> open(
        sstring path, std::function<void(const request&)> replay);
    // Creates a new, empty log in the file path, replacing any file there.
    static future<std::unique_ptr<append_only_log>> create(sstring path);
    // Appends a request, made of these words, in RESP. It isn't on the
    // disk until a commit.
    template <typename Words>
    void append(const Words& words) {
        format_request(words, [this] (std::string_view s) { copy(s); });
        _appends++;
    }
    // Writes the requests appended so far to the disk, and waits until the
    // disk has them. Commits that overlap share one write.
    future<> commit();
    // Writes the requests appended so far to the file, without waiting
    // until the disk has them for sure: for a log that needn't be durable
    // yet, such as a rewrite's new log, before it replaces the old one.
    future<> write();
    uint64_t uncommitted() const { return _size - _written; }
    uint64_t appends() const { return _appends; }
    // Commits what's left, and closes the file.
    future<> close();
    // At the end of a rewrite: makes next, written to the file tmp_path,
    // this log's replacement, at path. From then on, a commit of this log
    // commits next instead.
    future<> hand_over(append_only_log& next, sstring tmp_path, sstring path);
private:
    void copy(std::string_view s);
    future<> write_tail();
};
// end snippet: log

// Checks that dir's logs were written by as many shards as Shoal has now,
// since a key's shard depends on their number; for a new dir, records it.
future<> check_shard_count(sstring dir);
