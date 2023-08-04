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
#include <seastar/core/seastar.hh>
#include <seastar/util/log.hh>

namespace {
seastar::logger logger("word_count");

using WordCountMap = std::unordered_map<std::string, size_t>;
using WordCountMapPtr = std::unique_ptr<WordCountMap>;

class WordCount {
public:
  WordCount(seastar::file file, size_t startOffset, size_t estimatedBytesToRead)
      : _file{std::move(file)}, _offset{startOffset},
        _estimatedBytesToRead{estimatedBytesToRead} {}

  seastar::future<WordCountMapPtr> process() {
    return seastar::repeat([this] {
             return seastar::do_with(
                 seastar::allocate_aligned_buffer<char>(kBufferSize,
                                                        kBufferSize),
                 [this](auto &buffer) {
                   return _file.dma_read(_offset, buffer.get(), kBufferSize)
                       .then([this, &buffer](size_t bytesRead) {
                         logger.info("Read {} bytes", bytesRead);
                         if (bytesRead == 0) {
                           return seastar::stop_iteration::yes;
                         }

                         _offset += bytesRead;
                         _totalBytesRead += bytesRead;

                         _populateWordCount(buffer.get(), bytesRead);

                         if (_totalBytesRead >= _estimatedBytesToRead &&
                             !_readForPendingNewline) {
                           return seastar::stop_iteration::yes;
                         }

                         return seastar::stop_iteration::no;
                       });
                 });
           })
        .then([this] {
          return seastar::make_ready_future<WordCountMapPtr>(
              std::move(_wordCountMap));
        });
  }

private:
  void _populateWordCount(const char *buffer, size_t bytesRead) {
    std::string word;
    char readChar = '\0';

    const size_t startIdx = _getStartBufferIndex(buffer, bytesRead);
    for (size_t idx = startIdx; idx < bytesRead; ++idx) {
      readChar = buffer[idx];

      if (readChar != ' ' && readChar != '\n') {
        word += readChar;
        continue;
      }

      if (!word.empty()) {
        (*_wordCountMap)[word]++;
        word.clear();
      }

      if (_readForPendingNewline) {
        break;
      }
    }

    if (!word.empty()) {
      (*_wordCountMap)[word]++;
    }

    _readForPendingNewline = readChar != '\n';
  }

  size_t _getStartBufferIndex(const char *buffer, size_t bytesRead) {
    const bool isFirstRead = _isFirstRead;
    _isFirstRead = false;

    size_t idx = 0;
    if (!isFirstRead || buffer[idx] != '\n') {
      return idx;
    }

    const char *notFound = buffer + bytesRead;
    const char *result = std::find(buffer + idx + 1, buffer + bytesRead, '\n');
    return result == notFound ? bytesRead : result - buffer + 1;
  }

  static constexpr size_t kBufferSize = 4096;

  seastar::file _file;

  size_t _offset;
  const size_t _estimatedBytesToRead = 0;
  size_t _totalBytesRead = 0;

  bool _isFirstRead = true;
  bool _readForPendingNewline = false;

  WordCountMapPtr _wordCountMap = std::make_unique<WordCountMap>();
};
} // namespace

int main(int argc, char **argv) {
  seastar::app_template app;
  return app.run(argc, argv, [] {
    return seastar::open_file_dma("./large_file.txt", seastar::open_flags::ro)
        .then([](seastar::file f) {
          return seastar::do_with(
              WordCount(std::move(f), 0, 100), [](WordCount &wordCount) {
                return wordCount.process().then([](WordCountMapPtr map) {
                  for (const auto &pair : *map) {
                    std::cout << pair.first << " " << pair.second << std::endl;
                  }
                });
              });
        });
  });
}
