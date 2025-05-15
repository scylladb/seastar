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
 * Copyright (C) 2023 ScyllaDB Ltd.
 */

#include <iostream>
#include <memory>
#include <seastar/core/aligned_buffer.hh>
#include <seastar/core/app-template.hh>
#include <seastar/core/fstream.hh>
#include <seastar/core/iostream.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/when_all.hh>
#include <seastar/util/log.hh>

namespace {
seastar::logger logger("word_count");

// The page size for doing i/o operations.
static constexpr size_t k_page_size = 4096;

/**
 * A class that represents a task that is allocated to each 'word_count_processor' and processes
 * characters from a segment of a file. The class perform word counting in a map-reduce fashion.
 */
class word_count_task {
public:
    using ptr = std::unique_ptr<word_count_task>;
    using word_count_map = std::unordered_map<std::string, size_t>;
    using result = std::unique_ptr<word_count_map>;

    /**
     * Processes a character of a file and updates the word count map. The word count map tracks
     * count for each word.
     */
    void map(const char& ch) {
        static constexpr const char* k_delimiters = " \n.,:;";

        if (std::strchr(k_delimiters, ch) == nullptr) {
            _word += ch;
            return;
        }

        if (!_word.empty()) {
            (*_word_count_map)[_word]++;
            _word.clear();
        }
    }

    /**
     * Reduces the results of two tasks into a single task and returns the reduced task.
     */
    static ptr reduce(ptr&& task_left, ptr&& task_right) {
        auto reduced_task = create();
        auto& reduce_map = *reduced_task->_word_count_map;

        for (auto&& task : { std::move(task_left), std::move(task_right) }) {
            auto& wc_task = dynamic_cast<word_count_task&>(*task);
            for (auto&& [word, count] : *wc_task._word_count_map) { reduce_map[word] += count; }
        }

        return reduced_task;
    }

    /**
     * Releases the ownership of the word count map from the task.
     */
    result&& release() { return std::move(_word_count_map); }

    /**
     * Creates an instance of the word count task.
     */
    static ptr create() { return std::make_unique<word_count_task>(); }

private:
    // Keeps track of the current word being processed.
    std::string _word;

    // Maps each word to its count.
    result _word_count_map = std::make_unique<word_count_map>();
};

/**
 * A class that processes a segment of the provided file. A segment is a chunk of a file whose
 * starting offset is determined by the 'start_offset' and size by the 'estimated_bytes_to_read'.
 *
 * Each word count processor runs in a separate shard and invoke 'word_count_task::map' on each
 * character to perform mapper task.
 *
 * The processor is designed to handle words in a file that may be partitioned across
 * multiple word count processors.
 *
 * For instance, consider the sentence: "An apple is a fruit.\nOne should eat apple every day."
 * Say, the sentence is allocated to two processors as follows:
 *   Processor 1: "An apple is a fru"
 *   Processor 2: "it. One should eat apple every day."
 *
 * In this scenario, Processor 1 reads beyond the word 'fru' until it encounters a newline,
 * and Processor 2 begins processing characters after the newline. Consequently,
 *   Processor 1 processes: "An apple is a fruit."
 *   Processor 2 processes: "One should eat apple every day."
 *
 * NOTE: The processor can read more than the provided segment size provided by the
 * 'estimated_bytes_to_read'.
 */
class word_count_processor {
public:
    word_count_processor(const std::string& file_path, const size_t& start_offset,
                         const size_t& estimated_bytes_to_read, word_count_task::ptr&& task)
        : _file_path{ file_path },
          _start_offset{ start_offset },
          _estimated_bytes_to_read{ estimated_bytes_to_read },
          _task{ std::move(task) } {}

    /**
     * Processes the file and returns a future that is ready when the file is processed.
     */
    seastar::future<word_count_task::ptr> process() {
        return seastar::open_file_dma(_file_path, seastar::open_flags::ro)
            .then([this](auto file_desc) {
                _input_stream = std::move(make_file_input_stream(std::move(file_desc)));
                return _input_stream.skip(_start_offset).then([this] {
                    return seastar::repeat([this] {
                               return _input_stream.read_exactly(k_page_size)
                                   .then([this](auto&& buffer) {
                                       return _process_buffer(buffer.get(), buffer.size()) ?
                                                  seastar::stop_iteration::yes :
                                                  seastar::stop_iteration::no;
                                   });
                           })
                        .then([this] {
                            return seastar::make_ready_future<word_count_task::ptr>(
                                std::move(_task));
                        });
                });
            });
    }

private:
    /**
     * Processes the provided buffer and returns true if the file processing is complete.
     */
    bool _process_buffer(const char* buffer, const size_t& buffer_size) {
        if (buffer_size == 0) { return true; }

        // Determines if the file processing is complete or not.
        auto is_complete = false;

        // The offset of the buffer where the processing should start.
        auto buffer_offset = _get_buffer_start_offset(buffer, buffer_size);

        // If the total bytes read is less than the estimated bytes to read, then
        // continue processing.
        if (_total_bytes_read < _estimated_bytes_to_read) {
            std::tie(is_complete, buffer_offset)
                = _process_chars(buffer, buffer_size, buffer_offset);
        }

        // Even if all characters of the segment are processed, a segment is considered not
        // fully processed until the last character read is not new line character.
        if (!is_complete && _total_bytes_read >= _estimated_bytes_to_read) {
            std::tie(is_complete, buffer_offset)
                = _populate_remaining(buffer, buffer_size, buffer_offset);
        }

        return is_complete;
    }

    /**
     * Returns the offset of the buffer where the processing should start.
     */
    size_t _get_buffer_start_offset(const char* buffer, const size_t& buffer_size) {
        if (_start_offset == 0 || _total_bytes_read > 0) { return 0; }

        // If the start offset lies in the middle of a line, then skip all characters until a new
        // line character is found.
        const char* not_found = buffer + buffer_size;
        const char* result = std::find(buffer, buffer + buffer_size, '\n');
        auto offset = result == not_found ? buffer_size : result - buffer + 1;
        _total_bytes_read += offset;
        return offset;
    }

    /**
     * Processes characters from the buffer until the end of the buffer is reached or the total
     * bytes read is equal to the estimated bytes to read.
     */
    std::pair<bool, size_t> _process_chars(const char* buffer, const size_t& buffer_size,
                                           size_t offset) {
        char ch = '\0';
        for (; offset < buffer_size && _total_bytes_read < _estimated_bytes_to_read; offset++) {
            ch = buffer[offset];
            _invoke_task_processor(ch);
        }

        return { ch == '\n', offset };
    }

    /**
     * Processes any remaining characters in the buffer until a new line character is found.
     */
    std::pair<bool, size_t> _populate_remaining(const char* buffer, const size_t& buffer_size,
                                                size_t offset) {
        bool is_complete = false;
        for (; offset < buffer_size; offset++) {
            const char ch = buffer[offset];
            _invoke_task_processor(ch);

            if (ch == '\n') {
                is_complete = true;
                break;
            }
        }

        return { is_complete, offset };
    }

    /**
     * Invokes the task processor mapper function to process the provided character.
     */
    void _invoke_task_processor(const char& ch) {
        _total_bytes_read++;
        _task->map(ch);
    }

private:
    // The path to the file to process.
    const std::string _file_path;

    // The offset of the file where the processing should start.
    const size_t _start_offset;

    // The estimated number of bytes to read from the file.
    const size_t _estimated_bytes_to_read;

    // The total number of bytes read from the file.
    size_t _total_bytes_read = 0;

    // The input stream to read the file.
    seastar::input_stream<char> _input_stream;

    // The task that is allocated to this processor.
    word_count_task::ptr _task;
};

using word_count_processor_ptr = std::unique_ptr<word_count_processor>;

/**
 * A class that runs the word count processing on a provided file. This class is responsible for
 * splitting the file into segments, where a segment is a chunk of a file and each shard is
 * allocated a segment of file. Each shard runs a word count processor on its segment of the file.
 * The word count task from each shard is finally reduced to a single word count task. The result
 * from the final word count task is returned to the caller if the file processing is successful,
 * otherwise a null pointer is returned.
 */
class word_count_runner {
public:
    word_count_runner() {
        _app.add_options()("file_path", boost::program_options::value<std::string>()->required(),
                           "path to the file to process");
    }

    /**
     * Runs the word count processing on the provided file and returns the result if the file
     * processing is successful, otherwise a null pointer is returned.
     */
    word_count_task::result run(int argc, char** argv) {
        auto status = _app.run(argc, argv, [this] {
            static constexpr const char* k_file_path_key = "file_path";

            auto&& config = _app.configuration();
            const auto file_path = config[k_file_path_key].as<std::string>();

            return seastar::file_stat(file_path).then(
                [this, file_path](seastar::stat_data&& stat) -> seastar::future<> {
                    return _run(file_path, stat.size);
                });
        });

        return status == 0 ? std::move(_result) : nullptr;
    }

private:
    /**
     * Runs the word count processor on each shard and finally reduces the results from all shard to
     * single word count task.
     */
    seastar::future<> _run(std::string file_path, size_t file_size) {
        return _file_metadata.start(file_path, file_size)
            .then([this] {
                return _file_metadata.map_reduce0(
                    [this](auto file_metadata) {
                        auto [file_path, file_size] = file_metadata;
                        auto processor
                            = _create_processor(seastar::this_shard_id(), file_path, file_size);

                        return seastar::do_with(std::move(processor), [](auto& processor) {
                            return processor->process().then(
                                [](auto&& task) { return std::move(task); });
                        });
                    },
                    word_count_task::create(), word_count_task::reduce);
            })
            .then([this](auto&& task) {
                _result = std::move(task->release());
                return _file_metadata.stop();
            });
    }

    /**
     * Creates a word count processor for the provided core id, file path and file size.
     */
    word_count_processor_ptr _create_processor(size_t core_id, std::string file_path,
                                               size_t file_size) {
        // Calculate the estimated chunk size aligned to the page size.
        const auto estimated_chunk_size = [&]() {
            size_t chunk_size = file_size / seastar::smp::count;
            chunk_size = chunk_size / k_page_size * k_page_size;
            return chunk_size > k_page_size ? chunk_size : k_page_size;
        }();

        const size_t offset = core_id * estimated_chunk_size;
        const size_t estimated_bytes_to_read
            = core_id != seastar::smp::count - 1 ?
                  estimated_chunk_size :
                  file_size - (seastar::smp::count - 2) * estimated_chunk_size;

        return std::make_unique<word_count_processor>(file_path, offset, estimated_bytes_to_read,
                                                      word_count_task::create());
    }

    // The seastar application.
    seastar::app_template _app;

    // The file metadata that is copied to each shard.
    seastar::sharded<std::tuple<std::string, size_t>> _file_metadata;

    // The result of the word count processing.
    word_count_task::result _result;
};

} // namespace

/**
 * This is a demo application that performs word count on a provided file.
 *
 * Usage: word_count_demo --file_path <path to file>
 *
 * The algorithm is a follows:
 * 1. The 'word_count_runner.run()' method is invoked to run the word count processing on the file.
 * 2. The provided file is then divided equally into chunks called segments, and each segment is
 * allocated to each seastar shard.
 * 3. Each shard runs a 'word_count_processor' on its segment of the file. The job of the
 * 'word_count_processor' is to process characters on its segment of the file.
 * 4. Each 'word_count_processor' is assigned a 'word_count_task' that is responsible for performing
 * the word count.
 * 5. After successful processing for each shard, the results from each shard are reduced to a
 * single 'word_count_task' using 'word_count_task::reduce'.
 * 6. The final reduced 'word_count_task' releases a std::map that contains the word count.
 * 7. The resultant std::map data is then printed to the console.
 *
 */
int main(int argc, char** argv) {
    seastar::app_template app;

    word_count_runner runner{};
    auto result = runner.run(argc, argv);
    if (!result) {
        logger.error("Failed to fetch word count result.");
        return 1;
    }

    std::stringstream sstream;
    size_t total_words = 0;

    for (auto&& [word, count] : *result) {
        sstream << "  " << word << ": " << count << std::endl;
        total_words += count;
    }

    sstream << "Total words: " << total_words << std::endl;
    logger.info("\nWord count report:\n{}", sstream.str());
}
