/*
 * This file is open source software, licensed to you under the terms
 * of the Apache License, Version 2.0 (the "License").  See the NOTICE file
 * distributed with this work for additional information regarding copyright
 * ownership.  You may not use this file except in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
/*
 * Copyright 2024 ScyllaDB
 */

#pragma once

#ifndef SEASTAR_MODULE
#include <cstdint>
#include <exception>
#include <queue>
#include <string_view>
#include <vector>
#endif
#include <seastar/core/file.hh>
#include <seastar/core/future.hh>
#include <seastar/core/lowres_clock.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/core/sstring.hh>
#include <seastar/core/timer.hh>

namespace seastar {

namespace internal {

// The discard procedure has different phases.
// Firstly, the user queues the request into file_discard_queue.
// Then the content of queue is processed - discards are issued according to the given bandwidth.
// At some point all discard requests are issued.
// Empty files are a special case - they do not require any discards to be issued.
// If any error happens, then the procedure can be cancelled.
enum class file_discard_phase : uint8_t {
    queued = 0,
    in_progress = 1,
    all_requests_issued = 2,
    no_requests_needed = 3,
    cancelled = 4,
};

// When a file is removed via blocks discarding, then the whole operation may
// consist of multiple requests that span over time. Therefore, an internal
// data structure that describes the progress of the process is used. When the
// procedure is finished, then _discard_completed promise is marked as ready.
struct file_discard_context {
    sstring _filename{};
    file _file{};
    uint64_t _file_size{};
    uint32_t _next_block_idx{};
    uint32_t _in_flight_requests_count{};
    file_discard_phase _phase{file_discard_phase::queued};
    promise<> _discard_completed{};
    std::exception_ptr _exception{};

    file_discard_context(std::string_view filename)
        : _filename{filename}
    {}
};

} // namespace internal

// To construct file_discard_queue the user needs to supply the configuration.
// It describes the size of a single block that is discarded, as well as the
// number of discard requests issued per second.
// The maximum discard bandwidth per second can be calculated as:
//     DISCARD_BW = (_discard_block_size * _requests_per_second).
//
// NOTE: the actual discard bandwidth may be lower e.g. when the discarded file
// size is not divisible by _discard_block_size and the last discard request uses
// smaller block to cover the whole file.
struct file_discard_queue_config {
    uint64_t _discard_block_size{};
    uint64_t _requests_per_second{};
};

// To avoid I/O latency spikes that are caused by too many TRIM requests
// issued to the drive over a short period of time when the filesystem
// is mounted with the online discard option and many files are removed,
// the file_discard_queue class is used.
//
// Instead of relying on the unlink operation that trims all extents of
// a file at once, the approach that utilizes blocks discarding at a
// given pace is used.
//
// When a user wants to remove a file, then file_discard_queue::enqueue_file_removal()
// needs to be called. Its main purpose is to store the information about the work
// related to file discard in the internal data of the class.
//
// Reactor invokes file_discard_queue::try_discard_blocks() function via poller.
// The function checks if the bandwidth of discards allows another block to be discarded.
// If the bandwidth is available, then the discard is performed.
//
// The described approach ensures, that the amount of removed data remains at the configured
// level.
//
// NOTE: please be informed, that if a file uses extents, that are much smaller than the
//       discard block size, then one discard may map to multiple TRIM requests. It may
//       impact the I/O latency.
class file_discard_queue {
private:
    // Configuration of the available bandwidth.
    file_discard_queue_config _config;

    // Because the number of discard requests per second is limited, the timer is used
    // to reset the counter of available requests to given RPS value after 1 second.
    uint64_t _available_requests{};
    timer<lowres_clock> _refill_available_requests_timer;

    // The work that has not been started yet is stored in _enqueued_files.
    // The files that are being discarded at the moment are tracked via _currently_discarded_files.
    std::queue<lw_shared_ptr<internal::file_discard_context>> _enqueued_files{};
    std::vector<lw_shared_ptr<internal::file_discard_context>> _currently_discarded_files{};

    // Callback installed for a timer. Resets the available requests count to configured
    // _requests_per_second.
    void refill_available_requests() noexcept;

    // When the discard procedure finishes or is cancelled for a given file, then the context
    // contains the information about that fact. Before issuing discards in a reactor tick,
    // the context elements for completed discards are removed from the container.
    void clear_completed_discards();

    // Tries to schedule as many discards as possible for a given file.
    void schedule_discards(lw_shared_ptr<internal::file_discard_context> context);

    // Schedules a single discard for a given file according to the passed parameters.
    // The discard future has continuation that propagates the results to the original caller if needed.
    static void schedule_single_discard(lw_shared_ptr<internal::file_discard_context> context, uint64_t offset, uint64_t length);

    // Opens a given file, gets its size and schedules a single discard operation if file is not empty.
    // In the case when any of operations fails, propagates the error to the original caller.
    static void start_discard_procedure(lw_shared_ptr<internal::file_discard_context> context, uint64_t block_size);

    // If there are no more requests in flight, then performs the last step in the discard procedure.
    // The last step is closing the file that belongs to the context and setting the promise.
    static future<> maybe_close_discarded_file(lw_shared_ptr<internal::file_discard_context> context);

public:
    // Creates a discard queue that works according to the defined parameters.
    // Requires positive RPS value and positive block size aligned to 4KiB.
    file_discard_queue(const file_discard_queue_config& config);

    // The discard queue is neither copyable nor moveable type.
    file_discard_queue(const file_discard_queue& other) = delete;
    file_discard_queue(file_discard_queue&& other) = delete;
    file_discard_queue& operator=(const file_discard_queue& other) = delete;
    file_discard_queue& operator=(file_discard_queue&& other) = delete;

    // Destroys the queue.
    // The file discard queue cannot be destroyed when it has pending requests.
    // The user must wait until all futures returned from enqueue_file_removal()
    // are completed.
    ~file_discard_queue();

    // Stores the information about the need for the removal of a file.
    // Returns a future that is made ready when the enqueued file is removed.
    future<> enqueue_file_removal(std::string_view pathname) noexcept;

    // Invoked by reactor poller via poll().
    // If there is any work pending and there is available bandwidth,
    // then it discards blocks until the bandwidth is exhausted or
    // no more work is left.
    // Returns true if any discard was performed, else returns false.
    bool try_discard_blocks();

    // Invoked by reactor poller via pure_poll().
    // Does not perform any actual work related to issuing discard requests.
    // Checks if there is any work pending and available bandwidth.
    // If the work can be performed, then returns true. Else returns false.
    bool can_perform_work();
};

} // namespace seastar
