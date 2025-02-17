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
 * Copyright (C) 2015 Cloudius Systems, Ltd.
 */

#include <algorithm>
#include <iostream>
#include <numeric>
#include <seastar/core/fstream.hh>
#include <seastar/core/smp.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/core/app-template.hh>
#include <seastar/core/do_with.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/semaphore.hh>
#include <seastar/testing/random.hh>
#include <seastar/testing/test_case.hh>
#include <seastar/testing/test_runner.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/print.hh>
#include <seastar/util/assert.hh>
#include <seastar/util/defer.hh>
#include <seastar/util/tmp_file.hh>
#include <boost/range/adaptor/transformed.hpp>
#include <boost/algorithm/cxx11/any_of.hpp>
#include "mock_file.hh"
#include <boost/range/irange.hpp>
#include <seastar/util/closeable.hh>
#include <seastar/util/alloc_failure_injector.hh>

using namespace seastar;
namespace fs = std::filesystem;

struct writer {
    output_stream<char> out;
    static future<shared_ptr<writer>> make(file f) {
        return make_file_output_stream(std::move(f)).then([] (output_stream<char>&& os) {
            return make_shared<writer>(writer{std::move(os)});
        });
    }
};

struct reader {
    input_stream<char> in;
    reader(file f) : in(make_file_input_stream(std::move(f))) {}
    reader(file f, file_input_stream_options options) : in(make_file_input_stream(std::move(f), std::move(options))) {}
};

SEASTAR_TEST_CASE(test_fstream) {
    return tmp_dir::do_with([] (tmp_dir& t) {
        auto filename = (t.get_path() / "testfile.tmp").native();
        return open_file_dma(filename,
                open_flags::rw | open_flags::create | open_flags::truncate).then([filename] (file f) {
            return writer::make(std::move(f)).then([filename] (shared_ptr<writer> w) {
                auto buf = static_cast<char*>(::malloc(4096));
                memset(buf, 0, 4096);
                buf[0] = '[';
                buf[1] = 'A';
                buf[4095] = ']';
                return w->out.write(buf, 4096).then([buf, w] {
                    ::free(buf);
                    return make_ready_future<>();
                }).then([w] {
                    auto buf = static_cast<char*>(::malloc(8192));
                    memset(buf, 0, 8192);
                    buf[0] = '[';
                    buf[1] = 'B';
                    buf[8191] = ']';
                    return w->out.write(buf, 8192).then([buf, w] {
                        ::free(buf);
                        return w->out.close().then([w] {});
                    });
                }).then([filename] {
                    return open_file_dma(filename, open_flags::ro);
                }).then([] (file f) {
                    /*  file content after running the above:
                     * 00000000  5b 41 00 00 00 00 00 00  00 00 00 00 00 00 00 00  |[A..............|
                     * 00000010  00 00 00 00 00 00 00 00  00 00 00 00 00 00 00 00  |................|
                     * *
                     * 00000ff0  00 00 00 00 00 00 00 00  00 00 00 00 00 00 00 5d  |...............]|
                     * 00001000  5b 42 00 00 00 00 00 00  00 00 00 00 00 00 00 00  |[B..............|
                     * 00001010  00 00 00 00 00 00 00 00  00 00 00 00 00 00 00 00  |................|
                     * *
                     * 00002ff0  00 00 00 00 00 00 00 00  00 00 00 00 00 00 00 5d  |...............]|
                     * 00003000
                     */
                    auto r = make_shared<reader>(std::move(f));
                    return r->in.read_exactly(4096 + 8192).then([r] (temporary_buffer<char> buf) {
                        auto p = buf.get();
                        BOOST_REQUIRE(p[0] == '[' && p[1] == 'A' && p[4095] == ']');
                        BOOST_REQUIRE(p[4096] == '[' && p[4096 + 1] == 'B' && p[4096 + 8191] == ']');
                        return make_ready_future<>();
                    }).then([r] {
                        return r->in.close();
                    }).finally([r] {});
                });
            });
        });
    });
}

SEASTAR_TEST_CASE(test_consume_skip_bytes) {
    return tmp_dir::do_with_thread([] (tmp_dir& t) {
        auto filename = (t.get_path() / "testfile.tmp").native();
        auto f = open_file_dma(filename,
                               open_flags::rw | open_flags::create | open_flags::truncate).get();
        auto w = writer::make(std::move(f)).get();
        auto write_block = [w] (char c, size_t size) {
            std::vector<char> vec(size, c);
            w->out.write(&vec.front(), vec.size()).get();
        };
        write_block('a', 8192);
        write_block('b', 8192);
        w->out.close().get();
        /*  file content after running the above:
         * 00000000  61 61 61 61 61 61 61 61  61 61 61 61 61 61 61 61  |aaaaaaaaaaaaaaaa|
         * *
         * 00002000  62 62 62 62 62 62 62 62  62 62 62 62 62 62 62 62  |bbbbbbbbbbbbbbbb|
         * *
         * 00004000
         */
        f = open_file_dma(filename, open_flags::ro).get();
        auto r = make_lw_shared<reader>(std::move(f), file_input_stream_options{512});
        auto close_r_in = deferred_close(r->in);
        struct consumer {
            uint64_t _count = 0;
            using consumption_result_type = typename input_stream<char>::consumption_result_type;
            using stop_consuming_type = typename consumption_result_type::stop_consuming_type;
            using tmp_buf = stop_consuming_type::tmp_buf;

            /*
             * Consumer reads the file as follows:
             *  - first 8000 bytes are read in 512-byte chunks and checked
             *  - next 2000 bytes are skipped (jumping over both read buffer size and DMA block)
             *  - the remaining 6384 bytes are read and checked
             */
            future<consumption_result_type> operator()(tmp_buf buf) {
                if (_count < 8000) {
                    auto delta = std::min(buf.size(), 8000 - _count);
                    for (auto c : buf.share(0, delta)) {
                        BOOST_REQUIRE_EQUAL(c, 'a');
                    }
                    buf.trim_front(delta);
                    _count += delta;

                    if (_count == 8000) {
                        return make_ready_future<consumption_result_type>(skip_bytes{2000 - buf.size()});
                    } else {
                        SEASTAR_ASSERT(buf.empty());
                        return make_ready_future<consumption_result_type>(continue_consuming{});
                    }
                    return make_ready_future<consumption_result_type>(continue_consuming{});
                } else {
                    for (auto c : buf) {
                        BOOST_REQUIRE_EQUAL(c, 'b');
                    }
                    _count += buf.size();
                    if (_count < 14384) {
                        return make_ready_future<consumption_result_type>(continue_consuming{});
                    } else if (_count > 14384) {
                        BOOST_FAIL("Read more than expected");
                    }
                    return make_ready_future<consumption_result_type>(stop_consuming_type({}));
                }
            }
        };
        r->in.consume(consumer{}).get();
    });
}

SEASTAR_TEST_CASE(test_fstream_unaligned) {
  return tmp_dir::do_with([] (tmp_dir& t) {
    auto filename = (t.get_path() / "testfile.tmp").native();
    return open_file_dma(filename,
            open_flags::rw | open_flags::create | open_flags::truncate).then([filename] (file f) {
        return writer::make(std::move(f)).then([filename] (shared_ptr<writer> w) {
            auto buf = static_cast<char*>(::malloc(40));
            memset(buf, 0, 40);
            buf[0] = '[';
            buf[1] = 'A';
            buf[39] = ']';
            return w->out.write(buf, 40).then([buf, w] {
                ::free(buf);
                return w->out.close().then([w] {});
            }).then([filename] {
                return open_file_dma(filename, open_flags::ro);
            }).then([] (file f) {
                return do_with(std::move(f), [] (file& f) {
                    return f.size().then([] (size_t size) {
                        // assert that file was indeed truncated to the amount of bytes written.
                        BOOST_REQUIRE(size == 40);
                        return make_ready_future<>();
                    });
                });
            }).then([filename] {
                return open_file_dma(filename, open_flags::ro);
            }).then([] (file f) {
                auto r = make_shared<reader>(std::move(f));
                return r->in.read_exactly(40).then([r] (temporary_buffer<char> buf) {
                    auto p = buf.get();
                    BOOST_REQUIRE(p[0] == '[' && p[1] == 'A' && p[39] == ']');
                    return make_ready_future<>();
                }).then([r] {
                    return r->in.close();
                }).finally([r] {});
            });
        });
    });
  });
}

future<> test_consume_until_end(uint64_t size) {
  return tmp_dir::do_with([size] (tmp_dir& t) {
    auto filename = (t.get_path() / "testfile.tmp").native();
    return open_file_dma(filename,
            open_flags::rw | open_flags::create | open_flags::truncate).then([size] (file f) {
          return make_file_output_stream(f).then([size] (output_stream<char>&& os) {
            return do_with(std::move(os), [size] (output_stream<char>& out) {
                std::vector<char> buf(size);
                std::iota(buf.begin(), buf.end(), 0);
                return out.write(buf.data(), buf.size()).then([&out] {
                   return out.flush();
                });
          });
            }).then([f] {
                return f.size();
            }).then([size, f] (size_t real_size) {
                BOOST_REQUIRE_EQUAL(size, real_size);
            }).then([size, f] {
                auto consumer = [offset = uint64_t(0), size] (temporary_buffer<char> buf) mutable -> future<input_stream<char>::unconsumed_remainder> {
                    if (!buf) {
                        return make_ready_future<input_stream<char>::unconsumed_remainder>(temporary_buffer<char>());
                    }
                    BOOST_REQUIRE(offset + buf.size() <= size);
                    std::vector<char> expected(buf.size());
                    std::iota(expected.begin(), expected.end(), offset);
                    offset += buf.size();
                    BOOST_REQUIRE(std::equal(buf.begin(), buf.end(), expected.begin()));
                    return make_ready_future<input_stream<char>::unconsumed_remainder>(std::nullopt);
                };
                return do_with(make_file_input_stream(f), std::move(consumer), [] (input_stream<char>& in, auto& consumer) {
                    return in.consume(consumer).then([&in] {
                        return in.close();
                    });
                });
            }).finally([f] () mutable {
                return f.close();
            });
    });
  });
}


SEASTAR_TEST_CASE(test_consume_aligned_file) {
    return test_consume_until_end(4096);
}

SEASTAR_TEST_CASE(test_consume_empty_file) {
    return test_consume_until_end(0);
}

SEASTAR_TEST_CASE(test_consume_unaligned_file) {
    return test_consume_until_end(1);
}

SEASTAR_TEST_CASE(test_consume_unaligned_file_large) {
    return test_consume_until_end((1 << 20) + 1);
}

SEASTAR_TEST_CASE(test_input_stream_esp_around_eof) {
    return tmp_dir::do_with_thread([] (tmp_dir& t) {
        auto flen = uint64_t(5341);
        auto rdist = std::uniform_int_distribution<int>(0, std::numeric_limits<char>::max());
        auto reng = testing::local_random_engine;
        auto data = boost::copy_range<std::vector<uint8_t>>(
                boost::irange<uint64_t>(0, flen)
                | boost::adaptors::transformed([&] (int x) { return rdist(reng); }));
        auto filename = (t.get_path() / "testfile.tmp").native();
        auto f = open_file_dma(filename,
                open_flags::rw | open_flags::create | open_flags::truncate).get();
        auto close_f = deferred_close(f);
        auto out = make_file_output_stream(f).get();
        out.write(reinterpret_cast<const char*>(data.data()), data.size()).get();
        out.flush().get();
        //out.close().get();  // FIXME: closes underlying stream:?!
        struct range { uint64_t start; uint64_t end; };
        auto ranges = std::vector<range>{{
            range{0, flen},
            range{0, flen * 2},
            range{0, flen + 1},
            range{0, flen - 1},
            range{0, 1},
            range{1, 2},
            range{flen - 1, flen},
            range{flen - 1, flen + 1},
            range{flen, flen + 1},
            range{flen + 1, flen + 2},
            range{1023, flen-1},
            range{1023, flen},
            range{1023, flen + 2},
            range{8193, 8194},
            range{1023, 1025},
            range{1023, 1024},
            range{1024, 1025},
            range{1023, 4097},
        }};
        auto opt = file_input_stream_options();
        opt.buffer_size = 512;
        for (auto&& r : ranges) {
            auto start = r.start;
            auto end = r.end;
            auto len = end - start;
            auto in = make_file_input_stream(f, start, len, opt);
            std::vector<uint8_t> readback;
            auto more = true;
            while (more) {
                auto rdata = in.read().get();
                for (size_t i = 0; i < rdata.size(); ++i) {
                    readback.push_back(rdata.get()[i]);
                }
                more = !rdata.empty();
            }
            //in.close().get();
            auto xlen = std::min(end, flen) - std::min(flen, start);
            if (xlen != readback.size()) {
                BOOST_FAIL(format("Expected {:d} bytes but got {:d}, start={:d}, end={:d}", xlen, readback.size(), start, end));
            }
            BOOST_REQUIRE(std::equal(readback.begin(), readback.end(), data.begin() + std::min(start, flen)));
        }
    });
}

SEASTAR_TEST_CASE(without_api_prefix) {
    return tmp_dir::do_with_thread([](tmp_dir& t) {
        auto filename = (t.get_path() / "testfile.tmp").native();
        auto f = open_file_dma(filename,
                open_flags::rw | open_flags::create | open_flags::truncate).get();
        output_stream<char> out = make_file_output_stream(f).get();
        out.close().get();
    });
}

SEASTAR_TEST_CASE(file_handle_test) {
    return tmp_dir::do_with_thread([] (tmp_dir& t) {
        auto filename = (t.get_path() / "testfile.tmp").native();
        auto f = open_file_dma(filename, open_flags::create | open_flags::truncate | open_flags::rw).get();
        auto close_f = deferred_close(f);
        auto buf = static_cast<char*>(aligned_alloc(4096, 4096));
        auto del = defer([&] () noexcept { ::free(buf); });
        for (unsigned i = 0; i < 4096; ++i) {
            buf[i] = i;
        }
        f.dma_write(0, buf, 4096).get();
        auto bad = std::vector<unsigned>(smp::count); // std::vector<bool> is special and unsuitable because it uses bitfields
        smp::invoke_on_all([fh = f.dup(), &bad] {
            return seastar::async([fh, &bad] {
                auto f = fh.to_file();
                auto buf = static_cast<char*>(aligned_alloc(4096, 4096));
                auto del = defer([&] () noexcept { ::free(buf); });
                f.dma_read(0, buf, 4096).get();
                for (unsigned i = 0; i < 4096; ++i) {
                    bad[this_shard_id()] |= buf[i] != char(i);
                }
            });
        }).get();
        BOOST_REQUIRE(!boost::algorithm::any_of_equal(bad, 1u));
    });
}

SEASTAR_TEST_CASE(test_fstream_slow_start) {
    return seastar::async([] {
        static constexpr size_t file_size = 128 * 1024 * 1024;
        static constexpr size_t buffer_size = 260 * 1024;
        static constexpr size_t read_ahead = 1;

        auto mock_file = make_shared<mock_read_only_file>(file_size);

        auto history = make_lw_shared<file_input_stream_history>();

        file_input_stream_options options{};
        options.buffer_size = buffer_size;
        options.read_ahead = read_ahead;
        options.dynamic_adjustments = history;

        static constexpr size_t requests_at_slow_start = 2; // 1 request + 1 read-ahead
        static constexpr size_t requests_at_full_speed = read_ahead + 1; // 1 request + read_ahead

        std::optional<size_t> initial_read_size;

        auto read_whole_file_with_slow_start = [&] (auto fstr) {
            uint64_t total_read = 0;
            size_t previous_buffer_length = 0;

            // We don't want to assume too much about fstream internals, but with
            // no history we should start with a buffer sizes somewhere in
            // (0, buffer_size) range.
            mock_file->set_read_size_verifier([&] (size_t length) {
                BOOST_CHECK_LE(length, initial_read_size.value_or(buffer_size - 1));
                BOOST_CHECK_GE(length, initial_read_size.value_or(1));
                previous_buffer_length = length;
                if (!initial_read_size) {
                    initial_read_size = length;
                }
            });

            // Slow start phase
            while (true) {
                // We should leave slow start before reading the whole file.
                BOOST_CHECK_LT(total_read, file_size);

                mock_file->set_allowed_read_requests(requests_at_slow_start);
                auto buf = fstr.read().get();
                BOOST_CHECK_GT(buf.size(), 0u);

                mock_file->set_read_size_verifier([&] (size_t length) {
                    // There is no reason to reduce buffer size.
                    BOOST_CHECK_LE(length, std::min(previous_buffer_length * 2, buffer_size));
                    BOOST_CHECK_GE(length, previous_buffer_length);
                    previous_buffer_length = length;
                });

                BOOST_TEST_MESSAGE(format("Size {:d}", buf.size()));
                total_read += buf.size();
                if (buf.size() == buffer_size) {
                    BOOST_TEST_MESSAGE("Leaving slow start phase.");
                    break;
                }
            }

            // Reading at full speed now
            mock_file->set_expected_read_size(buffer_size);
            while (total_read != file_size) {
                mock_file->set_allowed_read_requests(requests_at_full_speed);
                auto buf = fstr.read().get();
                total_read += buf.size();
            }

            mock_file->set_allowed_read_requests(requests_at_full_speed);
            auto buf = fstr.read().get();
            BOOST_CHECK_EQUAL(buf.size(), 0u);
            SEASTAR_ASSERT(buf.size() == 0);
        };

        auto read_while_file_at_full_speed = [&] (auto fstr) {
            uint64_t total_read = 0;

            mock_file->set_expected_read_size(buffer_size);
            while (total_read != file_size) {
                mock_file->set_allowed_read_requests(requests_at_full_speed);
                auto buf = fstr.read().get();
                total_read += buf.size();
            }

            mock_file->set_allowed_read_requests(requests_at_full_speed);
            auto buf = fstr.read().get();
            BOOST_CHECK_EQUAL(buf.size(), 0u);
        };

        auto read_and_skip_a_lot = [&] (auto fstr) {
            uint64_t total_read = 0;
            size_t previous_buffer_size = buffer_size;

            mock_file->set_allowed_read_requests(std::numeric_limits<size_t>::max());
            mock_file->set_read_size_verifier([&] (size_t length) {
                // There is no reason to reduce buffer size.
                BOOST_CHECK_LE(length, previous_buffer_size);
                BOOST_CHECK_GE(length, initial_read_size.value_or(1));
                previous_buffer_size = length;
            });
            while (total_read != file_size) {
                auto buf = fstr.read().get();
                total_read += buf.size();

                buf = fstr.read().get();
                total_read += buf.size();

                auto skip_by = std::min(file_size - total_read, buffer_size * 2);
                fstr.skip(skip_by).get();
                total_read += skip_by;
            }

            // We should be back at slow start at this stage.
            BOOST_CHECK_LT(previous_buffer_size, buffer_size);
            if (initial_read_size) {
                BOOST_CHECK_EQUAL(previous_buffer_size, *initial_read_size);
            }

            mock_file->set_allowed_read_requests(requests_at_full_speed);
            auto buf = fstr.read().get();
            BOOST_CHECK_EQUAL(buf.size(), 0u);

        };

        auto make_fstream = [&] {
            struct fstream_wrapper {
                input_stream<char> s;
                explicit fstream_wrapper(input_stream<char>&& s) : s(std::move(s)) {}
                fstream_wrapper(fstream_wrapper&&) = default;
                fstream_wrapper& operator=(fstream_wrapper&&) = default;
                future<temporary_buffer<char>> read() {
                    return s.read();
                }
                future<> skip(uint64_t n) {
                    return s.skip(n);
                }
                ~fstream_wrapper() {
                    s.close().get();
                }
            };
            return fstream_wrapper(make_file_input_stream(file(mock_file), 0, file_size, options));
        };

        BOOST_TEST_MESSAGE("Reading file, no history, expectiong a slow start");
        read_whole_file_with_slow_start(make_fstream());
        BOOST_TEST_MESSAGE("Reading file again, everything good so far, read at full speed");
        read_while_file_at_full_speed(make_fstream());
        BOOST_TEST_MESSAGE("Reading and skipping a lot");
        read_and_skip_a_lot(make_fstream());
        BOOST_TEST_MESSAGE("Reading file, bad history, we are back at slow start...");
        read_whole_file_with_slow_start(make_fstream());
        BOOST_TEST_MESSAGE("Reading file yet again, should've recovered by now");
        read_while_file_at_full_speed(make_fstream());
    });
}

#ifdef SEASTAR_ENABLE_ALLOC_FAILURE_INJECTION

SEASTAR_TEST_CASE(test_close_error) {
    using namespace seastar::memory;

    return tmp_dir::do_with_thread([] (tmp_dir& t) {
        bool done = false;
        for (size_t i = 0; !done; i++) {
            bool got_close_error = false;
            sstring filename = (t.get_path() / format("testfile-{}.tmp", i).c_str()).native();
            file f = open_file_dma(filename, open_flags::rw | open_flags::create | open_flags::truncate).get();
            auto opts = file_output_stream_options{};
            opts.write_behind = 16;
            std::unique_ptr<output_stream<char>> out = std::make_unique<output_stream<char>>(make_file_output_stream(std::move(f), opts).get());
            size_t size = 4096;
            std::vector<char> buf(size);
            std::iota(buf.begin(), buf.end(), 0);
            size_t file_length = 1 * 1024 * 1024;
            auto fut = make_ready_future<>();
            for (size_t len = 0; len < file_length; len += size) {
                fut = fut.finally([&] { return out->write(buf.data(), size); });
            }
            fut.get();
            try {
                local_failure_injector().fail_after(i);
                out->close().get();
                done = true;
                local_failure_injector().cancel();
            } catch (const std::bad_alloc&) {
                got_close_error = true;
            }
            BOOST_REQUIRE(got_close_error || done);
            out.reset();
            remove_file(filename).get();
        }
    });
}

#endif // SEASTAR_ENABLE_ALLOC_FAILURE_INJECTION
