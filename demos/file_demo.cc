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
 * Copyright 2020 ScyllaDB
 */


// Demonstration of seastar::with_file

#include <cstring>
#include <limits>
#include <random>

#include <seastar/core/app-template.hh>

#include <seastar/core/aligned_buffer.hh>
#include <seastar/core/file.hh>
#include <seastar/core/fstream.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/sstring.hh>
#include <seastar/core/temporary_buffer.hh>
#include <seastar/core/loop.hh>
#include <seastar/core/io_intent.hh>
#include <seastar/util/assert.hh>
#include <seastar/util/log.hh>
#include <seastar/util/tmp_file.hh>

using namespace seastar;

constexpr size_t aligned_size = 4096;

future<> verify_data_file(file& f, temporary_buffer<char>& rbuf, const temporary_buffer<char>& wbuf) {
    return f.dma_read(0, rbuf.get_write(), aligned_size).then([&rbuf, &wbuf] (size_t count) {
        SEASTAR_ASSERT(count == aligned_size);
        fmt::print("    verifying {} bytes\n", count);
        SEASTAR_ASSERT(!memcmp(rbuf.get(), wbuf.get(), aligned_size));
    });
}

future<file> open_data_file(sstring meta_filename, temporary_buffer<char>& rbuf) {
    fmt::print("    retrieving data filename from {}\n", meta_filename);
    return with_file(open_file_dma(meta_filename, open_flags::ro), [&rbuf] (file& f) {
        return f.dma_read(0, rbuf.get_write(), aligned_size).then([&rbuf] (size_t count) {
            SEASTAR_ASSERT(count == aligned_size);
            auto data_filename = sstring(rbuf.get());
            fmt::print("    opening {}\n", data_filename);
            return open_file_dma(data_filename, open_flags::ro);
        });
    });
}

future<> demo_with_file() {
    fmt::print("Demonstrating with_file():\n");
    return tmp_dir::do_with_thread([] (tmp_dir& t) {
        auto rnd = std::mt19937(std::random_device()());
        auto dist = std::uniform_int_distribution<int>(0, std::numeric_limits<char>::max());
        auto wbuf = temporary_buffer<char>::aligned(aligned_size, aligned_size);
        sstring meta_filename = (t.get_path() / "meta_file").native();
        sstring data_filename = (t.get_path() / "data_file").native();

        // `with_file` is used to create/open `filename` just around the call to `dma_write`
        auto write_to_file = [] (const sstring filename, temporary_buffer<char>& wbuf) {
            auto count = with_file(open_file_dma(filename, open_flags::rw | open_flags::create), [&wbuf] (file& f) {
                return f.dma_write(0, wbuf.get(), aligned_size);
            }).get();
            SEASTAR_ASSERT(count == aligned_size);
        };

        // print the data_filename into the write buffer
        std::fill(wbuf.get_write(), wbuf.get_write() + aligned_size, 0);
        std::copy(data_filename.cbegin(), data_filename.cend(), wbuf.get_write());

        // and write it to `meta_filename`
        fmt::print("  writing \"{}\" into {}\n", data_filename, meta_filename);

        write_to_file(meta_filename, wbuf);

        // now write some random data into data_filename
        fmt::print("  writing random data into {}\n", data_filename);
        std::generate(wbuf.get_write(), wbuf.get_write() + aligned_size, [&dist, &rnd] { return dist(rnd); });

        write_to_file(data_filename, wbuf);

        // verify the data via meta_filename
        fmt::print("  verifying data...\n");
        auto rbuf = temporary_buffer<char>::aligned(aligned_size, aligned_size);

        with_file(open_data_file(meta_filename, rbuf), [&rbuf, &wbuf] (file& f) {
            return verify_data_file(f, rbuf, wbuf);
        }).get();
    });
}

future<> demo_with_file_close_on_failure() {
    fmt::print("\nDemonstrating with_file_close_on_failure():\n");
    return tmp_dir::do_with_thread([] (tmp_dir& t) {
        auto rnd = std::mt19937(std::random_device()());
        auto dist = std::uniform_int_distribution<int>(0, std::numeric_limits<char>::max());
        auto wbuf = temporary_buffer<char>::aligned(aligned_size, aligned_size);
        sstring meta_filename = (t.get_path() / "meta_file").native();
        sstring data_filename = (t.get_path() / "data_file").native();

        // with_file_close_on_failure will close the opened file only if
        // `make_file_output_stream` returns an error. Otherwise, in the error-free path,
        // the opened file is moved to `file_output_stream` that in-turn closes it
        // when the stream is closed.
        auto make_output_stream = [] (std::string_view filename) {
            return with_file_close_on_failure(open_file_dma(filename, open_flags::rw | open_flags::create), [] (file f) {
                return make_file_output_stream(std::move(f), aligned_size);
            });
        };

        // writes the buffer one byte at a time, to demonstrate output stream
        auto write_to_stream = [] (output_stream<char>& o, const temporary_buffer<char>& wbuf) {
            return seastar::do_for_each(wbuf, [&o] (char c) {
                return o.write(&c, 1);
            }).finally([&o] {
                return o.close();
            });
        };

        // print the data_filename into the write buffer
        std::fill(wbuf.get_write(), wbuf.get_write() + aligned_size, 0);
        std::copy(data_filename.cbegin(), data_filename.cend(), wbuf.get_write());

        // and write it to `meta_filename`
        fmt::print("  writing \"{}\" into {}\n", data_filename, meta_filename);

        // with_file_close_on_failure will close the opened file only if
        // `make_file_output_stream` returns an error. Otherwise, in the error-free path,
        // the opened file is moved to `file_output_stream` that in-turn closes it
        // when the stream is closed.
        output_stream<char> o = make_output_stream(meta_filename).get();

        write_to_stream(o, wbuf).get();

        // now write some random data into data_filename
        fmt::print("  writing random data into {}\n", data_filename);
        std::generate(wbuf.get_write(), wbuf.get_write() + aligned_size, [&dist, &rnd] { return dist(rnd); });

        o = make_output_stream(data_filename).get();

        write_to_stream(o, wbuf).get();

        // verify the data via meta_filename
        fmt::print("  verifying data...\n");
        auto rbuf = temporary_buffer<char>::aligned(aligned_size, aligned_size);

        with_file(open_data_file(meta_filename, rbuf), [&rbuf, &wbuf] (file& f) {
            return verify_data_file(f, rbuf, wbuf);
        }).get();
    });
}

static constexpr size_t half_aligned_size = aligned_size / 2;

future<> demo_with_io_intent() {
    fmt::print("\nDemonstrating demo_with_io_intent():\n");
    return tmp_dir::do_with_thread([] (tmp_dir& t) {
        sstring filename = (t.get_path() / "testfile.tmp").native();
        auto f = open_file_dma(filename, open_flags::rw | open_flags::create).get();

        auto rnd = std::mt19937(std::random_device()());
        auto dist = std::uniform_int_distribution<int>(0, std::numeric_limits<char>::max());

        auto wbuf = temporary_buffer<char>::aligned(aligned_size, aligned_size);
        fmt::print("  writing random data into {}\n", filename);
        std::generate(wbuf.get_write(), wbuf.get_write() + aligned_size, [&dist, &rnd] { return dist(rnd); });

        f.dma_write(0, wbuf.get(), aligned_size).get();

        auto wbuf_n = temporary_buffer<char>::aligned(aligned_size, aligned_size);
        fmt::print("  starting to overwrite {} with other random data in two steps\n", filename);
        std::generate(wbuf_n.get_write(), wbuf_n.get_write() + aligned_size, [&dist, &rnd] { return dist(rnd); });

        io_intent intent;
        auto f1 = f.dma_write(0, wbuf_n.get(), half_aligned_size);
        auto f2 = f.dma_write(half_aligned_size, wbuf_n.get() + half_aligned_size, half_aligned_size, &intent);

        fmt::print("  cancel the 2nd overwriting\n");
        intent.cancel();

        fmt::print("  wait for overwriting IOs to complete\n");
        f1.get();

        bool cancelled = false;
        try {
            f2.get();
            // The file::dma_write doesn't preemt, but if it
            // suddenly will, the 2nd write will pass before
            // the intent would be cancelled
            fmt::print("    2nd write won the race with cancellation\n");
        } catch (cancelled_error& ex) {
            cancelled = true;
        }

        fmt::print("  verifying data...\n");
        auto rbuf = allocate_aligned_buffer<unsigned char>(aligned_size, aligned_size);
        f.dma_read(0, rbuf.get(), aligned_size).get();

        // First part of the buffer must coincide with the overwritten data
        SEASTAR_ASSERT(!memcmp(rbuf.get(), wbuf_n.get(), half_aligned_size));

        if (cancelled) {
            // Second part -- with the old data ...
            SEASTAR_ASSERT(!memcmp(rbuf.get() + half_aligned_size, wbuf.get() + half_aligned_size, half_aligned_size));
        } else {
            // ... or with new if the cancellation didn't happen
            SEASTAR_ASSERT(!memcmp(rbuf.get() + half_aligned_size, wbuf.get() + half_aligned_size, half_aligned_size));
        }
    });
}

int main(int ac, char** av) {
    app_template app;
    return app.run(ac, av, [] {
        return demo_with_file().then([] {
            return demo_with_file_close_on_failure().then([] {
                return demo_with_io_intent();
            });
        });
    });
}
