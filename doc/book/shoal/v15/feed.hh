// Replication (version 14): a shard's feed, the changes that a replica
// fetches from the primary.
#pragma once

#include "resp.hh"
#include <seastar/core/condition-variable.hh>
#include <seastar/core/lowres_clock.hh>
#include <seastar/core/temporary_buffer.hh>
#include <cstdint>
#include <deque>
#include <string_view>

// snippet: feed
// A shard's changes, as the requests that made them, in RESP, the same
// requests that its log records. A byte's place in the feed, its offset,
// counts from the feed's start, when Shoal started. The feed keeps only
// the latest changes, at least limit bytes of them (--repl-backlog-size),
// in chunks: it drops the oldest chunk when it can do without it.
class replication_feed {
    static constexpr size_t chunk_size = 16 * 1024;
    // The chunks, all full but the last; the first begins at offset
    // _start, a multiple of chunk_size.
    std::deque<temporary_buffer<char>> _chunks;
    uint64_t _start = 0;
    uint64_t _end = 0;              // the offset after the last change
    uint64_t _fetched = 0;          // the offset of the latest fetch
    size_t _limit;
    uint64_t _snapshots = 0;        // the snapshots started so far
    condition_variable _grown;      // signaled when changes are appended
    condition_variable _fetching;   // signaled by every fetch
public:
    // A random number, which tells a replica whether this is the feed
    // that it fetched from, or a new one, of a primary that restarted.
    const uint64_t id;

    explicit replication_feed(size_t limit);
    // Appends a request, made of these words.
    template <typename Words>
    void append(const Words& words) {
        format_request(words, [this] (std::string_view s) { copy(s); });
        _grown.broadcast();
    }
    uint64_t end() const { return _end; }
    uint64_t fetched() const { return _fetched; }   // version 15
    uint64_t snapshots() const { return _snapshots; }
    size_t limit() const { return _limit; }
    // The changes from offset on, up to max bytes of them. If there are
    // none yet, waits for them until the deadline, and then returns
    // nothing. Throws if the feed no longer has the change at offset.
    future<sstring> fetch(uint64_t offset, size_t max,
                          lowres_clock::time_point deadline);
    // For a new snapshot: stops the previous one, if it's running, and
    // returns the new one's number.
    uint64_t start_snapshot();
    // Waits until the bytes not fetched yet are at most n, and returns
    // whether the snapshot of this number may go on: false once a newer
    // snapshot starts.
    future<bool> wait_for_fetch(size_t n, uint64_t snapshot);
    // Ends the waits of fetch() and wait_for_fetch(), as Shoal stops.
    void stop();
private:
    void copy(std::string_view s);
};
// end snippet: feed
