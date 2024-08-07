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

#include "create_file.hh"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <random>
#include <string_view>

#include <seastar/core/app-template.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/sleep.hh>
#include <seastar/util/defer.hh>
#include <seastar/util/file.hh>
#include <seastar/util/tmp_file.hh>

using namespace seastar;
using namespace std::chrono_literals;

using seastar::testing::create_file_with_size;

namespace {

constexpr const char* io_properties_yaml_format = R"(disks:
  - mountpoint: {}
    read_iops: 70000
    read_bandwidth: 800M
    write_iops: 30000
    write_bandwidth: 400M
    discard_block_size: 1MB
    discard_requests_per_second: 32
)";

std::filesystem::path get_tmpdir() {
    auto env = getenv("TMPDIR");
    return std::filesystem::path{env ? env : "/tmp"};
}

std::string random_dir_name() {
    static auto random_seed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    static std::default_random_engine random_generator(random_seed);

    std::uniform_int_distribution<int> rand_num(0, 3000000);
    return fmt::format("test-{}-{}.tmp", rand_num(random_generator), rand_num(random_generator));
}

std::vector<std::string> copy_argv(int argc, char** argv) {
    std::vector<std::string> result{};
    result.reserve(argc);

    for (int i = 0; i < argc; ++i) {
        result.push_back(argv[i]);
    }

    return result;
}

std::vector<char*> get_argv_view(std::vector<std::string>& argv) {
    std::vector<char*> result{};
    result.reserve(argv.size());

    for (auto& arg : argv) {
        result.push_back(arg.data());
    }

    return result;
}

std::vector<std::string> generate_file_paths(std::filesystem::path test_dir, size_t files_count) {
    std::vector<std::string> paths{};
    paths.reserve(files_count);

    for (auto i = 0u; i < files_count; ++i) {
        auto filename = fmt::format("shard-{}-file-{}.txt", this_shard_id(), i);
        paths.push_back((test_dir / filename).string());
    }

    return paths;
}

} // namespace

namespace tests {

future<> discard_2MiB_files_test(std::filesystem::path top_test_dir) {
    return tmp_dir::do_with(top_test_dir, [] (tmp_dir& td) {
        return async([test_dir = td.get_path()] () mutable {
            // Create 64 files, 2MiB each.
            constexpr size_t files_count{64u};
            constexpr uint64_t file_size{2u << 20u};

            const auto file_paths = generate_file_paths(test_dir, files_count);
            for (const auto& file_path : file_paths) {
                create_file_with_size(file_path, file_size).get();
                if (!file_exists(file_path).get()) {
                    auto error_msg = fmt::format("discard_2MiB_files_test: setup failed - could not create: {}", file_path);
                    throw std::runtime_error{error_msg};
                }
            }

            // The configured bandwidth for discard is 32MiB/s (32 requests per second, 1MiB block size) for each shard.
            // The test thread executes the test case independently on each shard (each one has independed file discard queue).
            // Therefore, at least 4 full rounds of issuing discard requests are needed (we have 64 files, 2MiB each).
            // Because the test case is isolated, the first round of discard requests can be issued without waiting.
            // Also, because RPS is refilled according to timer, that is periodic it may happen, that RPS will be refilled
            // right after issuing the first round of discard requests.
            // Thus, we are sure that at least 2 seconds are needed to issue all discards (even when refill happens right
            // after issuing the first bulk of requests, then we need additional 2 refills => at least 2s).
            auto removal_start = std::chrono::high_resolution_clock::now();

            // Enqueue removal of files via blocks discarding.
            std::vector<future<>> file_removed_futures{};
            file_removed_futures.reserve(files_count);

            for (const auto& file_path : file_paths) {
                file_removed_futures.emplace_back(remove_file_via_blocks_discarding(file_path));
            }

            // Wait for completion.
            for (auto& f : file_removed_futures) {
                f.get();
            }

            // Measure the elapsed time.
            auto removal_end = std::chrono::high_resolution_clock::now();
            auto elapsed_time = std::chrono::duration_cast<std::chrono::milliseconds>(removal_end - removal_start);

            // Ensure that the removal took at least 2 seconds.
            if (elapsed_time < 2000ms) {
                auto error_msg = fmt::format("discard_2MiB_files_test: removal should have taken at least 2000ms "
                                             "according to the config. Elapsed time: {}", elapsed_time.count());
                throw std::runtime_error{error_msg};
            }

            // Ensure that all files were removed.
            for (const auto& file_path : file_paths) {
                if (file_exists(file_path).get()) {
                    auto error_msg = fmt::format("discard_2MiB_files_test: expectation failed - file not removed: {}", file_path);
                    throw std::runtime_error{error_msg};
                }
            }
        });
    });
}

future<> empty_directory_passed_as_argument_test(std::filesystem::path top_test_dir) {
    return tmp_dir::do_with(top_test_dir, [] (tmp_dir& td) {
        return async([test_dir = td.get_path()] () mutable {
            auto empty_dir_path = (test_dir / "some_empty_dir").string();
            make_directory(empty_dir_path, file_permissions::default_dir_permissions).get();

            if (!file_exists(empty_dir_path).get()) {
                std::string error_msg = "empty_directory_passed_as_argument_test: precondition failed - could not create empty dir!";
                throw std::runtime_error{error_msg};
            }

            remove_file_via_blocks_discarding(empty_dir_path).get();

            if (file_exists(empty_dir_path).get()) {
                std::string error_msg = "empty_directory_passed_as_argument_test: expectation failed - empty dir not removed!";
                throw std::runtime_error{error_msg};
            }
        });
    });
}

future<> discard_100MiB_file_test(std::filesystem::path top_test_dir) {
    return tmp_dir::do_with(top_test_dir, [] (tmp_dir& td) {
        return async([test_dir = td.get_path()] () mutable {
            // Create 100MiB file.
            constexpr uint64_t file_size{100u << 20u};
            const auto filename = fmt::format("shard-{}-big-file.txt", this_shard_id());
            const auto file_path = (test_dir / filename).string();

            create_file_with_size(file_path, file_size).get();

            if (!file_exists(file_path).get()) {
                auto error_msg = fmt::format("discard_100MiB_file_test: setup failed - could not create: {}", file_path);
                throw std::runtime_error{error_msg};
            }

            // Measure the time required to discard the file.
            // The configured bandwidth for discard is 32MiB/s (32 requests per second, 1MiB block size) for each shard.
            // The size of a file that is discarded is 100 MiB => 3 full rounds and some fraction of 4th of issuing discards
            // are needed. Because the test case is isolated, the first round of discard requests can be issued without waiting.
            // Also, because RPS is refilled according to timer, that is periodic it may happen, that RPS will be refilled
            // right after issuing the first round of discard requests.
            // Therefore, the time needed to issue all discards is at least 2 second (even if the RPS is refilled right after
            // issuing the requests, we need to wait for another full round of issuing and then for the fraction of the next
            // round. Refill happens once per second).
            auto removal_start = std::chrono::high_resolution_clock::now();

            // Remove file.
            remove_file_via_blocks_discarding(file_path).get();

            // Measure the elapsed time.
            auto removal_end = std::chrono::high_resolution_clock::now();
            auto elapsed_time = std::chrono::duration_cast<std::chrono::milliseconds>(removal_end - removal_start);

            // Ensure that the removal took at least 2 seconds.
            if (elapsed_time < 2000ms) {
                auto error_msg = fmt::format("discard_100MiB_file_test: removal should have taken at least 2000ms "
                                             "according to the config. Elapsed time: {}", elapsed_time.count());
                throw std::runtime_error{error_msg};
            }

            // Ensure that file has been removed and it does not exist.
            if (file_exists(file_path).get()) {
                std::string error_msg = "discard_100MiB_file_test: expectation failed - 100MiB file not removed!";
                throw std::runtime_error{error_msg};
            }
        });
    });
}

} // namespace tests

// Because we want to configure global file discard queues for a given mountpoint
// via I/O properties, we need to do a few things before running the test cases:
//  1. Get the path of temporary directory in the same way that tmp_dir does.
//  2. Create a directory that the test cases will treat as the main test dir in the obtained path.
//  3. Prepare I/O properties YAML that provides configuration of file discard queue
//     with mountpoint set to the previosly created 'main' directory of the test.
//  4. Add I/O properties parameter to the command line passed to seastar application.
int main(int ac, char** av) {
    // Create a test-specific directory in TMPDIR.
    auto tmp_dir = get_tmpdir();
    auto current_test_dir = (tmp_dir / random_dir_name());
    if (!std::filesystem::create_directory(current_test_dir)) {
        std::cerr << "Could not create directory for the test case!" << std::endl;
        return -1;
    }

    // Install cleanup function to ensure that the dir is removed at the end of execution.
    auto test_dir_cleanup = defer([&]() noexcept {
        std::error_code error{};
        std::filesystem::remove_all(current_test_dir, error);
    });

    // Prepare I/O properties with mountpoint equal to the run directory of this particular test binary.
    std::string io_properties_yaml = fmt::format(io_properties_yaml_format, current_test_dir.string());

    // Perform a deep copy of arguments and append I/O properties.
    std::vector<std::string> argv = copy_argv(ac, av);
    argv.push_back("--io-properties");
    argv.push_back(io_properties_yaml);

    // Create a view of arguments and additionally append nullptr at the end to meet the requirements
    // of argv array.
    std::vector<char*> argv_view = get_argv_view(argv);
    argv_view.push_back(nullptr);

    // Do not include the terminator in the arguments count.
    int argc = static_cast<int>(argv_view.size() - 1u);

    // Run test cases one after another to ensure, that the usage of file_discard_queue per shard is isolated.
    return app_template().run(argc, argv_view.data(), [current_test_dir] {
        return async([current_test_dir] () mutable {
            // The following test case checks if the removal via blocks discarding
            // works according to the configured RPS and block size.
            //
            // The configuration applies to each shard - file_discard_queue objects
            // are members of reactor and are created per shard.
            smp::invoke_on_all([current_test_dir] {
                return tests::discard_2MiB_files_test(current_test_dir);
            }).get();

            // Ensure that available requests of file_discard_queues were refilled by expiration of timer.
            sleep(1s).get();

            // The following test case is used to verify the behavior when empty directory
            // is passed to the removal function. We cannot discard blocks from dir.
            // Therefore, just redirect the call to the usual remove_file().
            tests::empty_directory_passed_as_argument_test(current_test_dir).get();

            // Ensure that available requests of file_discard_queues were refilled by expiration of timer.
            sleep(1s).get();

            // The following test case tries to discard one big file according to the configured bandwidth.
            smp::invoke_on_all([current_test_dir] {
                return tests::discard_100MiB_file_test(current_test_dir);
            }).get();
        });
    });
}
