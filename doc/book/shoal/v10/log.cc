// A shard's append-only log.
#include "log.hh"
#include <seastar/core/align.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/fstream.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/smp.hh>
#include <seastar/coroutine/as_future.hh>
#include <seastar/coroutine/exception.hh>
#include <seastar/util/file.hh>
#include <seastar/util/later.hh>
#include <fmt/format.h>
#include <algorithm>
#include <charconv>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

// The size of the chunks that hold the requests not yet written: a
// multiple of any disk's alignment.
constexpr size_t chunk_size = 128 * 1024;

// snippet: open
future<std::unique_ptr<append_only_log>> append_only_log::open(
        sstring path, std::function<void(const request&)> replay) {
    auto log = std::make_unique<append_only_log>();
    log->_file = co_await open_file_dma(path, open_flags::rw |
                                        open_flags::create);
    // Read the requests back with Shoal's own parser, from a stream that
    // reads the file ahead of it, in large pieces.
    auto in = make_file_input_stream(log->_file, file_input_stream_options{
        .buffer_size = 128 * 1024, .read_ahead = 4});
    semaphore memory(std::numeric_limits<int32_t>::max());
    request_reader reader(in, memory, memory.available_units());
    std::exception_ptr error;
    try {
        while (auto req = co_await reader.read()) {
            replay(*req);
        }
    } catch (...) {
        error = std::current_exception();
    }
    co_await in.close();
    auto size = co_await log->_file.size();
    if (error) {
        // A crash in the middle of a write leaves a request cut short, or
        // a block of garbage: cut the file after the last whole request.
        shoal_log.warn("{}: {}; cutting off its last {} bytes", path,
                       formattable(error), size - reader.parsed_end());
        size = reader.parsed_end();
        co_await log->_file.truncate(size);
        co_await log->_file.flush();
    }
    // Go on appending after the last block. If the file ends in the middle
    // of one, start at the next: the bytes in between read as zeros, which
    // the parser skips, as it skips the padding of every commit.
    log->_size = log->_written = log->_tail_start =
        align_up(size, uint64_t(log->_file.disk_write_dma_alignment()));
    co_return log;
}
// end snippet: open

// snippet: append
// Copies s into the chunks, adding a chunk whenever the last one is full.
void append_only_log::copy(std::string_view s) {
    while (!s.empty()) {
        auto used = _size - _tail_start;
        if (used == _tail.size() * chunk_size) {
            _tail.push_back(temporary_buffer<char>::aligned(
                _file.memory_dma_alignment(), chunk_size));
        }
        auto n = std::min(s.size(), chunk_size - used % chunk_size);
        std::memcpy(_tail.back().get_write() + used % chunk_size, s.data(), n);
        s.remove_prefix(n);
        _size += n;
    }
}
// end snippet: append

// snippet: commit
future<> append_only_log::commit() {
    // This commit is for the requests appended so far. But first let the
    // requests being executed now, such as the rest of a pipeline, or of a
    // batch from another shard, append theirs, so that they share its
    // write. And a commit that waits for an earlier one may find that it
    // wrote this one's requests too.
    auto end = _size;
    co_await yield();
    auto units = co_await get_units(_committing, 1);
    if (_written < end) {
        co_await write_tail();
    }
}

// Writes the requests that aren't in the file yet, and flushes. The last
// block is padded with zeros to the disk's alignment, and the requests
// appended later begin after it, so that each block is written only once,
// and a crash in the middle of a write can't damage an earlier one.
future<> append_only_log::write_tail() {
    auto from = _written;
    auto to = align_up(_size, uint64_t(_file.disk_write_dma_alignment()));
    auto used = _size - _tail_start;
    std::memset(_tail.back().get_write() + used % chunk_size, 0, to - _size);
    _size = to;
    // Requests appended during the writes go after `to`, into memory that
    // the writes don't read.
    for (auto pos = from; pos < to; ) {
        auto& chunk = _tail[(pos - _tail_start) / chunk_size];
        auto in_chunk = (pos - _tail_start) % chunk_size;
        auto n = std::min<uint64_t>(to - pos, chunk_size - in_chunk);
        auto written = co_await _file.dma_write(pos, chunk.get() + in_chunk, n);
        if (written != n) {
            co_await coroutine::return_exception(
                std::runtime_error("short write to the log"));
        }
        pos += n;
    }
    co_await _file.flush();
    _written = to;
    while (!_tail.empty() && _tail_start + chunk_size <= _written) {
        _tail.pop_front();
        _tail_start += chunk_size;
    }
}
// end snippet: commit

future<> append_only_log::close() {
    auto committed = co_await coroutine::as_future(commit());
    co_await _file.close();
    co_await std::move(committed);
}

// snippet: shard-count
future<> check_shard_count(sstring dir) {
    auto path = dir + "/shards";
    auto shards = fmt::format("{}\n", this_smp_shard_count());
    if (co_await file_exists(path)) {
        auto recorded = co_await util::read_entire_file_contiguous(
            std::string(path));
        if (recorded != shards) {
            co_await coroutine::return_exception(std::runtime_error(
                fmt::format("{} was written by Shoal on {} shards; run it on "
                            "as many", dir, std::string_view(recorded).substr(
                                0, recorded.size() - 1))));
        }
        co_return;
    }
    // A new file appears whole, or not at all: write it under a temporary
    // name, make its contents durable, rename it, and make the rename
    // durable, by flushing the directory.
    auto tmp = path + ".tmp";
    auto f = co_await open_file_dma(tmp, open_flags::wo | open_flags::create |
                                         open_flags::truncate);
    auto out = co_await make_file_output_stream(std::move(f));
    co_await out.write(shards);
    co_await out.flush();
    co_await out.close();
    co_await rename_file(tmp, path);
    co_await sync_directory(dir);
}
// end snippet: shard-count
