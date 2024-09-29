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

#ifdef SEASTAR_MODULE
module;
#endif

#include <algorithm>
#include <cassert>
#include <chrono>
#include <stdexcept>

#include <fmt/format.h>

#ifdef SEASTAR_MODULE
module seastar;
#else
#include <seastar/core/do_with.hh>
#include <seastar/core/file.hh>
#include <seastar/core/file_discard_queue.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/when_all.hh>
#endif

using seastar::internal::file_discard_context;
using seastar::internal::file_discard_phase;

namespace seastar {

file_discard_queue::file_discard_queue(const file_discard_queue_config& config)
    : _config{config}
    , _available_requests{config._requests_per_second}
    , _refill_available_requests_timer{[this] { refill_available_requests(); }}
{
    if ((config._discard_block_size == 0u) || (config._discard_block_size % 4096u != 0u)) {
        throw std::invalid_argument{"Cannot construct file_discard_queue: block size must be positive and aligned to 4KiB!"};
    }

    if (_config._requests_per_second == 0u) {
        throw std::invalid_argument{"Cannot construct file_discard_queue: RPS must be positive!"};
    }

    _currently_discarded_files.reserve(std::max(4u * _config._requests_per_second, uint64_t{256u}));
    _refill_available_requests_timer.arm_periodic(std::chrono::seconds{1});
}

file_discard_queue::~file_discard_queue() {
    // Remove entries related to finished or cancelled files before performing final check.
    // It is needed, because the _currently_discarded_files container is filtered in the
    // function that is called by reactor's poller. It may happen, that the poller does not
    // make it on time (although all requests were issued and processed).
    if (!_currently_discarded_files.empty()) {
        clear_completed_discards();
    }

    // It is illegal to destroy the file discard queue if all requests have not been issued.
    // The application that utilizes seastar must wait for all the submitted futures to complete.
    // File discard queue will be destroyed only after reactor terminates, which will happen
    // only when there are no more fibers to run.
    assert(_enqueued_files.empty() && _currently_discarded_files.empty());
}

void file_discard_queue::refill_available_requests() noexcept {
    _available_requests = _config._requests_per_second;
}

future<> file_discard_queue::enqueue_file_removal(std::string_view pathname) noexcept {
    return futurize_invoke([this, pathname] {
        _enqueued_files.push(make_lw_shared<file_discard_context>(pathname));
        return _enqueued_files.back()->_discard_completed.get_future();
    });
}

bool file_discard_queue::can_perform_work() {
    // No bandwidth available - no work can be done.
    if (_available_requests == 0u) {
        return false;
    }

    // Remove entries related to finished or cancelled files.
    if (!_currently_discarded_files.empty()) {
        clear_completed_discards();
    }

    // We have bandwidth and files, for which discard procedure has not been started yet.
    // Some work can be done by starting the discard procedure.
    if (!_enqueued_files.empty()) {
        return true;
    }

    // We neither have queued files nor discards in progress.
    if (_currently_discarded_files.empty()) {
        return false;
    }

    // Check if there is a file discard in progress that requires issuing requests.
    for (const auto& context : _currently_discarded_files) {
        if (context->_phase == file_discard_phase::in_progress) {
            return true;
        }
    }

    // We only have files that are waiting for finishing discard startup (open, unlink, size).
    return false;
}

bool file_discard_queue::try_discard_blocks() {
    if (!can_perform_work()) {
        return false;
    }

    // Firstly, try to progress with files for which discard procedure has already been started.
    for (auto& context : _currently_discarded_files) {
        schedule_discards(context);
        if (_available_requests == 0u) {
            return true;
        }
    }

    // If any bandwidth left, then start the procedure for some waiting files.
    for (auto i = 0u; i < 8u; ++i) {
        if (_enqueued_files.empty() || _available_requests == 0u) {
            break;
        }

        _currently_discarded_files.push_back(std::move(_enqueued_files.front()));
        _enqueued_files.pop();

        start_discard_procedure(_currently_discarded_files.back(), _config._discard_block_size);

        // Because the size of a file is not known until the setup finishes, only single discard
        // is scheduled for that file right after it is opened.
        _available_requests--;
    }

    // Early call to can_perform_work() guarantees that the work was done.
    return true;
}

void file_discard_queue::clear_completed_discards() {
    const auto issuing_discards_ended = [](const auto& context) {
        return context->_phase > file_discard_phase::in_progress;
    };

    std::erase_if(_currently_discarded_files, issuing_discards_ended);
}

void file_discard_queue::schedule_discards(lw_shared_ptr<file_discard_context> context) {
    // Setup of discard procedure has not finished yet.
    if (context->_phase != file_discard_phase::in_progress) {
        return;
    }

    // Try to issue as many discard requests as possible.
    auto offset = context->_next_block_idx * _config._discard_block_size;
    while (_available_requests > 0u && offset < context->_file_size) {
        auto discard_end = std::min(offset + _config._discard_block_size, context->_file_size);
        auto length = discard_end - offset;

        schedule_single_discard(context, offset, length);
        _available_requests--;

        context->_next_block_idx++;
        offset = context->_next_block_idx * _config._discard_block_size;
    }

    // If all requests for a given file have been issued, then change the procedure phase.
    if (context->_file_size <= offset) {
        context->_phase = file_discard_phase::all_requests_issued;
    }
}

void file_discard_queue::schedule_single_discard(lw_shared_ptr<file_discard_context> context, uint64_t offset, uint64_t length) {
    // Current request issued.
    context->_in_flight_requests_count++;

    // Caller synchronizes using the future returned for context->_discard_completed.
    (void)context->_file.discard(offset, length).then_wrapped([context] (future<> f) {
        // Current request finished.
        context->_in_flight_requests_count--;

        // Another discard failed.
        if (context->_phase == file_discard_phase::cancelled) {
            return maybe_close_discarded_file(context);
        }

        // Current discard failed - inform other fibers.
        if (f.failed()) {
            context->_phase = file_discard_phase::cancelled;
            context->_exception = f.get_exception();
            return maybe_close_discarded_file(context);
        }

        // When all requests have been issued check if this is the last request.
        if (context->_phase == file_discard_phase::all_requests_issued) {
            return maybe_close_discarded_file(context);
        }

        // More requests required.
        return make_ready_future<>();
    });
}

future<> file_discard_queue::maybe_close_discarded_file(lw_shared_ptr<file_discard_context> context) {
    // Some work is still pending.
    if (context->_in_flight_requests_count > 0u) {
        return make_ready_future<>();
    }

    // Close the file and set the result via promise.
    return context->_file.close().then([context] () {
        if (context->_exception) {
            context->_discard_completed.set_exception(context->_exception);
        } else {
            context->_discard_completed.set_value();
        }

        return make_ready_future<>();
    });
}

void file_discard_queue::start_discard_procedure(lw_shared_ptr<file_discard_context> context, uint64_t block_size) {
    // Caller synchronizes using the future returned for context->_discard_completed.
    (void)open_file_dma(context->_filename, open_flags::rw, file_open_options{}).then_wrapped([block_size, context] (future<file> f) mutable {
        // The file could not be opened. Propagate the error.
        if (f.failed()) {
            context->_phase = file_discard_phase::cancelled;
            context->_discard_completed.set_exception(f.get_exception());
            return make_ready_future<>();
        }

        // The file lives as long as context->_file object holds its handle and is not closed.
        context->_file = f.get();

        return engine().remove_file(context->_filename).then_wrapped([block_size, context](future<> f) mutable {
            // Unlink failed. Close the file and propagate error.
            if (f.failed()) {
                context->_phase = file_discard_phase::cancelled;
                context->_exception = f.get_exception();
                return maybe_close_discarded_file(context);
            }

            return context->_file.size().then_wrapped([block_size, context](future<uint64_t> f) mutable {
                // Could not get file size. Close the file and propagate error.
                if (f.failed()) {
                    context->_phase = file_discard_phase::cancelled;
                    context->_exception = f.get_exception();
                    return maybe_close_discarded_file(context);
                }

                // If the file is empty, then no discard work is required. Close the file and signal status.
                auto file_size = f.get();
                if (file_size == 0u) {
                    context->_phase = file_discard_phase::no_requests_needed;
                    return maybe_close_discarded_file(context);
                }

                // Discard is issued only for the first block. When setup of discard procedure
                // was invoked, then we did not know the size of the file.
                auto offset = 0u;
                auto discard_end = std::min(offset + block_size, file_size);
                auto length = discard_end - offset;

                context->_file_size = file_size;
                context->_next_block_idx = 1u;

                if (context->_file_size <= block_size) {
                    // The file is smaller or equal to a single discard. Move directly to all_requests_issued phase.
                    context->_phase = file_discard_phase::all_requests_issued;
                } else {
                    // More discards need to be issued. Move to in_progress phase.
                    context->_phase = file_discard_phase::in_progress;
                }

                schedule_single_discard(context, offset, length);
                return make_ready_future<>();
            });
        });
    });
}

} // namespace seastar
