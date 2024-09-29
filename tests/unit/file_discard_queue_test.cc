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
 * Copyright (C) 2024 ScyllaDB
 */

#include <array>
#include <cstdint>
#include <cstring>
#include <chrono>
#include <optional>

#include "create_file.hh"

#include <seastar/testing/test_case.hh>
#include <seastar/testing/thread_test_case.hh>
#include <seastar/testing/test_runner.hh>

#include <seastar/core/file_discard_queue.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/sleep.hh>
#include <seastar/util/tmp_file.hh>

using namespace seastar;
using namespace std::chrono_literals;

using seastar::testing::create_file_with_size;

SEASTAR_TEST_CASE(queue_construction_requires_positive_rps_value) {
    // When trying to construct file discard queue with invalid RPS equals zero.
    std::optional<file_discard_queue> discard_queue{};
    file_discard_queue_config config{4096u, 0u};

    // Then exception is raised.
    BOOST_REQUIRE_EXCEPTION(discard_queue.emplace(config), std::invalid_argument,
        testing::exception_predicate::message_contains("Cannot construct file_discard_queue: RPS must be positive!"));

    return make_ready_future<>();
}

SEASTAR_TEST_CASE(queue_construction_requires_positive_block_size) {
    // When trying to construct file discard queue with invalid too small block size.
    std::optional<file_discard_queue> discard_queue{};
    file_discard_queue_config config{0u, 4u};

    // Then exception is raised.
    BOOST_REQUIRE_EXCEPTION(discard_queue.emplace(config), std::invalid_argument,
        testing::exception_predicate::message_contains("Cannot construct file_discard_queue: block size must be positive and aligned to 4KiB!"));

    return make_ready_future<>();
}

SEASTAR_TEST_CASE(queue_construction_requires_block_size_aligned_to_4KiB) {
    // When trying to construct file discard queue with unaligned block size.
    std::optional<file_discard_queue> discard_queue{};
    file_discard_queue_config config{512u, 4u};

    // Then exception is raised.
    BOOST_REQUIRE_EXCEPTION(discard_queue.emplace(config), std::invalid_argument,
        testing::exception_predicate::message_contains("Cannot construct file_discard_queue: block size must be positive and aligned to 4KiB!"));

    return make_ready_future<>();
}

SEASTAR_THREAD_TEST_CASE(discard_single_file_with_size_divisible_by_blocks) {
    tmp_dir::do_with_thread([] (tmp_dir& td) {
        // Configure queue to issue 2 discard requests with block size 8KiB.
        file_discard_queue_config config{8192, 2u};
        file_discard_queue tested_queue{config};

        // When no work has been queued, then work cannot be performed.
        BOOST_REQUIRE(!tested_queue.can_perform_work());

        // Create 40KiB file. In case when tested removal fails, it will be removed by the destructor of tmp_dir().
        const auto file_path = (td.get_path() / "my_file.txt").string();
        create_file_with_size(file_path, 40u * 1024u).get();
        BOOST_REQUIRE(file_exists(file_path).get());

        // Enqueue removal of file to the queue. Expect that progress can be made - we have bandwidth
        // and work to do. Then do work.
        auto file_removed_future = tested_queue.enqueue_file_removal(file_path);
        BOOST_REQUIRE(tested_queue.can_perform_work());
        BOOST_REQUIRE(tested_queue.try_discard_blocks());

        // Because the test thread did not yield, the actual work related to opening and unlinking file
        // was not done - it was only queued. Therefore, expect that we cannot perform any further
        // work - the only file that was queued waits for startup of discard procedure to finish.
        BOOST_REQUIRE(!tested_queue.can_perform_work());
        BOOST_REQUIRE(!tested_queue.try_discard_blocks());

        // Sleep to ensure that RPS timer was refilled and the startup (open + unlink + 1 discard) was finished.
        sleep(1s).get();

        // At this point the file should be opened, unlinked and 1 discard request should be issued.
        // From filesystem's POV the file was unlinked and should not be reachable.
        BOOST_REQUIRE(!file_exists(file_path).get());

        // Configured block size is 8KiB and the file size is 40KiB. Therefore, 4 more discard requests are
        // needed. Thus, expect that the returned file_removed_future is not ready. The future is marked ready
        // when all blocks have been discarded.
        BOOST_REQUIRE(!file_removed_future.available());

        // Now the discard procedure for the enqueued file is in "in_progress" phase.
        // Expect that work can be done and perform it.
        BOOST_REQUIRE(tested_queue.can_perform_work());
        BOOST_REQUIRE(tested_queue.try_discard_blocks());

        // Sleep to ensure that RPS timer was refilled and two more discards were finished (3 in total so far).
        sleep(1s).get();

        // 2 more discard requests are needed. Thus, expect that the returned file_removed_future is not ready.
        BOOST_REQUIRE(!file_removed_future.available());

        // Repeat work - last two discards shall be issued.
        BOOST_REQUIRE(tested_queue.can_perform_work());
        BOOST_REQUIRE(tested_queue.try_discard_blocks());

        // Wait for completion of the removal.
        BOOST_REQUIRE_NO_THROW(file_removed_future.get());
    }).get();
}

SEASTAR_THREAD_TEST_CASE(discard_two_files_with_size_not_divisible_by_block) {
    tmp_dir::do_with_thread([] (tmp_dir& td) {
        // Configure queue to issue 2 discard requests with block size 32KiB.
        file_discard_queue_config config{32u * 1024u, 2u};
        file_discard_queue tested_queue{config};

        // Create two 72KiB files. In case when tested removal fails, they will be removed by the destructor of tmp_dir().
        const std::array paths = {
            (td.get_path() / "my_file_1.txt").string(),
            (td.get_path() / "my_file_2.txt").string(),
        };

        for (const auto& p : paths) {
            create_file_with_size(p, 72u * 1024u).get();
            BOOST_REQUIRE(file_exists(p).get());
        }

        // Enqueue all files to be removed.
        std::array file_removed_futures = {
            tested_queue.enqueue_file_removal(paths[0]),
            tested_queue.enqueue_file_removal(paths[1]),
        };

        // Start the procedure for both files (due to RPS == 2).
        BOOST_REQUIRE(tested_queue.try_discard_blocks());

        // Sleep to ensure that RPS timer was refilled and the discard procedure was started for both.
        sleep(1s).get();

        // Because the discard procedure should be started at this point, the files should not be reachable
        // from the filesystem (unlink was called).
        BOOST_REQUIRE(!file_exists(paths[0]).get());
        BOOST_REQUIRE(!file_exists(paths[1]).get());

        // However, neither of files was fully discarded - we issued 1 request for each file.
        BOOST_REQUIRE(!file_removed_futures[0].available());
        BOOST_REQUIRE(!file_removed_futures[1].available());

        // Issue next discards.
        BOOST_REQUIRE(tested_queue.try_discard_blocks());

        // Sleep to ensure that RPS timer was refilled and the next 2 discards were issued for the first file.
        sleep(1s).get();

        // Because the files are processed in FIFO order and each file requires 3 discard requests to complete,
        // then expect that the first file was completely removed and the second one still requires processing.
        BOOST_REQUIRE(file_removed_futures[0].available());
        BOOST_REQUIRE_NO_THROW(file_removed_futures[0].get());

        BOOST_REQUIRE(!file_removed_futures[1].available());

        // Issue the remaining 2 discards for the second file.
        BOOST_REQUIRE(tested_queue.try_discard_blocks());

        // We expect now that the discards will be issued and when they finish, the future will be marked as ready.
        BOOST_REQUIRE_NO_THROW(file_removed_futures[1].get());
    }).get();
}

SEASTAR_THREAD_TEST_CASE(discard_three_files_with_size_smaller_than_block) {
    tmp_dir::do_with_thread([] (tmp_dir& td) {
        // Configure queue to issue 2 discard requests with block size 64KiB.
        file_discard_queue_config config{64u * 1024u, 2u};
        file_discard_queue tested_queue{config};

        // Create three 32KiB files. In case when tested removal fails, they will be removed by the destructor of tmp_dir().
        const std::array paths = {
            (td.get_path() / "my_file_1.txt").string(),
            (td.get_path() / "my_file_2.txt").string(),
            (td.get_path() / "my_file_3.txt").string(),
        };

        for (const auto& p : paths) {
            create_file_with_size(p, 32u * 1024u).get();
            BOOST_REQUIRE(file_exists(p).get());
        }

        // Enqueue all files to be removed.
        std::array file_removed_futures = {
            tested_queue.enqueue_file_removal(paths[0]),
            tested_queue.enqueue_file_removal(paths[1]),
            tested_queue.enqueue_file_removal(paths[2]),
        };

        // Start the procedure for the first two files (due to RPS == 2).
        BOOST_REQUIRE(tested_queue.try_discard_blocks());

        // Sleep to ensure that RPS timer was refilled and the first two files were discarded.
        sleep(1s).get();

        BOOST_REQUIRE(!file_exists(paths[0]).get());
        BOOST_REQUIRE(file_removed_futures[0].available());
        BOOST_REQUIRE_NO_THROW(file_removed_futures[0].get());

        BOOST_REQUIRE(!file_exists(paths[1]).get());
        BOOST_REQUIRE(file_removed_futures[1].available());
        BOOST_REQUIRE_NO_THROW(file_removed_futures[1].get());

        // The third file is waiting in the queue.
        BOOST_REQUIRE(file_exists(paths[2]).get());
        BOOST_REQUIRE(!file_removed_futures[2].available());

        // Start the procedure for the last file.
        BOOST_REQUIRE(tested_queue.try_discard_blocks());

        // Wait for it to finish and ensure that the file was discarded.
        BOOST_REQUIRE_NO_THROW(file_removed_futures[2].get());
        BOOST_REQUIRE(!file_exists(paths[2]).get());
    }).get();
}

SEASTAR_THREAD_TEST_CASE(discard_empty_file) {
    tmp_dir::do_with_thread([] (tmp_dir& td) {
        // Configure queue to issue 2 discard requests with block size 8KiB.
        file_discard_queue_config config{8192u, 2u};
        file_discard_queue tested_queue{config};

        // Create empty file. In case when tested removal fails, it will be removed by the destructor of tmp_dir().
        const auto file_path = (td.get_path() / "my_file.txt").string();
        create_file_with_size(file_path, 0u).get();
        BOOST_REQUIRE(file_exists(file_path).get());

        // Enqueue removal of file to the queue. Trigger the work.
        auto file_removed_future = tested_queue.enqueue_file_removal(file_path);
        BOOST_REQUIRE(tested_queue.try_discard_blocks());

        // Wait until the file is removed (during startup operation we check the size
        // and if it is zero, then no discard requests are issued).
        BOOST_REQUIRE_NO_THROW(file_removed_future.get());
    }).get();
}

SEASTAR_THREAD_TEST_CASE(discard_nonexistent_file) {
    tmp_dir::do_with_thread([] (tmp_dir& td) {
        // Configure queue to issue 2 discard requests with block size 8KiB.
        file_discard_queue_config config{8192u, 2u};
        file_discard_queue tested_queue{config};

        // Enqueue removal of a nonexistent file to the queue. Trigger the work.
        auto file_removed_future = tested_queue.enqueue_file_removal("nonexistent_file.txt");
        BOOST_REQUIRE(tested_queue.try_discard_blocks());

        // Expect that error is raised.
        BOOST_REQUIRE_THROW(file_removed_future.get(), std::exception);
    }).get();
}

SEASTAR_THREAD_TEST_CASE(enqueue_the_same_file_twice) {
    tmp_dir::do_with_thread([] (tmp_dir& td) {
        // Configure queue to issue 2 discard requests with block size 8KiB.
        file_discard_queue_config config{8192u, 2u};
        file_discard_queue tested_queue{config};

        // Create 24KiB file. In case when tested removal fails, it will be removed by the destructor of tmp_dir().
        const auto file_path = (td.get_path() / "my_file.txt").string();
        create_file_with_size(file_path, 24u * 1024u).get();
        BOOST_REQUIRE(file_exists(file_path).get());

        // Enqueue removal of the same file twice to the queue. Trigger the work.
        auto file_removed_future_1 = tested_queue.enqueue_file_removal(file_path);
        auto file_removed_future_2 = tested_queue.enqueue_file_removal(file_path);
        BOOST_REQUIRE(tested_queue.try_discard_blocks());

        // Sleep to ensure that RPS timer was refilled and the discard procedure was started for both entries.
        sleep(1s).get();

        // Ensure that one of the startup procedures gracefully failed. The remaining one should continue discarding blocks.
        // Even when removing the file without blocks discarding approach, std::remove() would fail for the second executed call.
        std::optional<future<>> expected_failed_future;
        std::optional<future<>> expected_successful_future;

        if (file_removed_future_1.available()) {
            expected_failed_future.emplace(std::move(file_removed_future_1));
            expected_successful_future.emplace(std::move(file_removed_future_2));
        } else {
            expected_failed_future.emplace(std::move(file_removed_future_2));
            expected_successful_future.emplace(std::move(file_removed_future_1));
        }

        BOOST_REQUIRE(expected_failed_future->available());
        BOOST_REQUIRE_THROW(expected_failed_future->get(), std::exception);
        BOOST_REQUIRE(!expected_successful_future->available());

        // Ensure that one of unlinks succeeded and the file is not visible from the filesystem's POV.
        BOOST_REQUIRE(!file_exists(file_path).get());

        // The file required 3 discard requests to be issued. So far only 1 was issued and performed.
        // Issue 2 remaining ones.
        BOOST_REQUIRE(tested_queue.try_discard_blocks());

        // Wait until the file is removed via the "in_progress" procedure.
        BOOST_REQUIRE_NO_THROW(expected_successful_future->get());
    }).get();
}
