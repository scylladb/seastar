#include "feed.hh"
#include <seastar/core/coroutine.hh>
#include <algorithm>
#include <cstring>
#include <random>
#include <stdexcept>

replication_feed::replication_feed(size_t limit)
    : _limit(limit), id(std::random_device()()) {}

// snippet: copy
// Copies s into the chunks, adding a chunk whenever the last one is full,
// and then drops the oldest chunks that the feed can do without.
void replication_feed::copy(std::string_view s) {
    while (!s.empty()) {
        auto used = _end % chunk_size;      // in the last chunk
        if (used == 0) {
            _chunks.emplace_back(chunk_size);
        }
        auto n = std::min(s.size(), chunk_size - used);
        std::memcpy(_chunks.back().get_write() + used, s.data(), n);
        s.remove_prefix(n);
        _end += n;
    }
    while (_end - _start >= _limit + chunk_size) {
        _chunks.pop_front();
        _start += chunk_size;
    }
}
// end snippet: copy

// snippet: fetch
future<sstring> replication_feed::fetch(uint64_t offset, size_t max,
                                        lowres_clock::time_point deadline) {
    if (offset > _end) {
        throw std::runtime_error("fetching beyond the end of the feed");
    }
    // The replica has everything before offset: a snapshot that waits for
    // it to fetch may go on.
    _fetched = offset;
    _fetching.broadcast();
    if (offset == _end) {
        try {
            co_await _grown.wait(deadline, [&] { return _end > offset; });
        } catch (condition_variable_timed_out&) {
            co_return sstring();
        }
    }
    // While it waited, the feed may have dropped the change at offset.
    if (offset < _start) {
        throw std::runtime_error(fmt::format(
            "the feed no longer has offset {}", offset));
    }
    auto n = std::min<uint64_t>(max, _end - offset);
    auto data = uninitialized_string(n);
    for (size_t copied = 0; copied < n; ) {
        auto at = offset + copied;
        auto& chunk = _chunks[(at - _start) / chunk_size];
        auto k = std::min(n - copied, chunk_size - at % chunk_size);
        std::memcpy(data.data() + copied, chunk.get() + at % chunk_size, k);
        copied += k;
    }
    co_return data;
}
// end snippet: fetch

uint64_t replication_feed::start_snapshot() {
    // Wakes the previous snapshot, if it's waiting, to see that it's over.
    _fetching.broadcast();
    return ++_snapshots;
}

future<bool> replication_feed::wait_for_fetch(size_t n, uint64_t snapshot) {
    co_await _fetching.wait([&] {
        return _end - _fetched <= n || _snapshots != snapshot;
    });
    co_return _snapshots == snapshot;
}

void replication_feed::stop() {
    _grown.broken();
    _fetching.broken();
}
