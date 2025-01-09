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
 * Copyright (C) 2020 ScyllaDB
 */

#include <stdlib.h>
#include <random>

#include <seastar/testing/random.hh>
#include <seastar/testing/test_case.hh>
#include <seastar/testing/thread_test_case.hh>
#include <seastar/testing/test_runner.hh>

#include <seastar/core/file.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/print.hh>
#include <seastar/core/loop.hh>
#include <seastar/util/tmp_file.hh>
#include <seastar/util/file.hh>

#include "expected_exception.hh"

using namespace seastar;
namespace fs = std::filesystem;

SEASTAR_TEST_CASE(test_make_tmp_file) {
    return make_tmp_file().then([] (tmp_file tf) {
        return async([tf = std::move(tf)] () mutable {
            const sstring tmp_path = tf.get_path().native();
            BOOST_REQUIRE(file_exists(tmp_path).get());
            tf.close().get();
            tf.remove().get();
            BOOST_REQUIRE(!file_exists(tmp_path).get());
        });
    });
}

static temporary_buffer<char> get_init_buffer(file& f) {
    auto buf = temporary_buffer<char>::aligned(f.memory_dma_alignment(), f.memory_dma_alignment());
    memset(buf.get_write(), 0, buf.size());
    return buf;
}

SEASTAR_THREAD_TEST_CASE(test_tmp_file) {
    size_t expected = ~0;
    size_t actual = 0;

    tmp_file::do_with([&] (tmp_file& tf) mutable {
        auto& f = tf.get_file();
        auto buf = get_init_buffer(f);
        return do_with(std::move(buf), [&] (auto& buf) mutable {
            expected = buf.size();
            return f.dma_write(0, buf.get(), buf.size()).then([&] (size_t written) {
                actual = written;
                return make_ready_future<>();
            });
        });
    }).get();
    BOOST_REQUIRE_EQUAL(expected , actual);
}

SEASTAR_THREAD_TEST_CASE(test_non_existing_TMPDIR) {
    BOOST_REQUIRE_EXCEPTION(tmp_file::do_with("/tmp/non-existing-TMPDIR", [] (tmp_file& tf) {}).get(),
            std::system_error, testing::exception_predicate::message_contains("No such file or directory"));
}

static future<> touch_file(const sstring& filename, open_flags oflags = open_flags::rw | open_flags::create) noexcept {
    return open_file_dma(filename, oflags).then([] (file f) {
        return f.close().finally([f] {});
    });
}

SEASTAR_THREAD_TEST_CASE(test_recursive_remove_directory) {
    struct test_dir {
        test_dir *parent;
        sstring name;
        std::list<sstring> sub_files = {};
        std::list<test_dir> sub_dirs = {};

        test_dir(test_dir* parent, sstring name)
            : parent(parent)
            , name(std::move(name))
        { }

        fs::path path() const {
            if (!parent) {
                return fs::path(name.c_str());
            }
            return parent->path() / name.c_str();
        }

        void fill_random_file(std::uniform_int_distribution<unsigned>& dist, std::default_random_engine& eng) {
            sub_files.emplace_back(format("file-{}", dist(eng)));
        }

        test_dir& fill_random_dir(std::uniform_int_distribution<unsigned>& dist, std::default_random_engine& eng) {
            sub_dirs.emplace_back(this, format("dir-{}", dist(eng)));
            return sub_dirs.back();
        }

        void random_fill(int level, int levels, std::uniform_int_distribution<unsigned>& dist, std::default_random_engine& eng) {
            int num_files = dist(eng) % 10;
            int num_dirs = (level < levels - 1) ? (1 + dist(eng) % 3) : 0;

            for (int i = 0; i < num_files; i++) {
                fill_random_file(dist, eng);
            }

            if (num_dirs) {
                level++;
                for (int i = 0; i < num_dirs; i++) {
                    fill_random_dir(dist, eng).random_fill(level, levels, dist, eng);
                }
            }
        }

        future<> populate() {
            return touch_directory(path().native()).then([this] {
                return parallel_for_each(sub_files, [this] (auto& name) {
                    return touch_file((path() / name.c_str()).native());
                }).then([this] {
                    return parallel_for_each(sub_dirs, [] (auto& sub_dir) {
                        return sub_dir.populate();
                    });
                });
            });
        }
    };

    auto& eng = testing::local_random_engine;
    auto dist = std::uniform_int_distribution<unsigned>();
    int levels = 1 + dist(eng) % 3;
    test_dir root = { nullptr, default_tmpdir().native() };
    test_dir base = { &root, format("base-{}", dist(eng)) };
    base.random_fill(0, levels, dist, eng);
    base.populate().get();
    recursive_remove_directory(base.path()).get();
    BOOST_REQUIRE(!file_exists(base.path().native()).get());
}

SEASTAR_TEST_CASE(test_make_tmp_dir) {
    return make_tmp_dir().then([] (tmp_dir td) {
        return async([td = std::move(td)] () mutable {
            const sstring tmp_path = td.get_path().native();
            BOOST_REQUIRE(file_exists(tmp_path).get());
            td.remove().get();
            BOOST_REQUIRE(!file_exists(tmp_path).get());
        });
    });
}

SEASTAR_THREAD_TEST_CASE(test_tmp_dir) {
    size_t expected;
    size_t actual;
    tmp_dir::do_with([&] (tmp_dir& td) {
        return tmp_file::do_with(td.get_path(), [&] (tmp_file& tf) {
            auto& f = tf.get_file();
            auto buf = get_init_buffer(f);
            return do_with(std::move(buf), [&] (auto& buf) mutable {
                expected = buf.size();
                return f.dma_write(0, buf.get(), buf.size()).then([&] (size_t written) {
                    actual = written;
                    return make_ready_future<>();
                });
            });
        });
    }).get();
    BOOST_REQUIRE_EQUAL(expected , actual);
}

SEASTAR_THREAD_TEST_CASE(test_tmp_dir_with_path) {
    size_t expected;
    size_t actual;
    tmp_dir::do_with(".", [&] (tmp_dir& td) {
        return tmp_file::do_with(td.get_path(), [&] (tmp_file& tf) {
            auto& f = tf.get_file();
            auto buf = get_init_buffer(f);
            return do_with(std::move(buf), [&] (auto& buf) mutable {
                expected = buf.size();
                return tf.get_file().dma_write(0, buf.get(), buf.size()).then([&] (size_t written) {
                    actual = written;
                    return make_ready_future<>();
                });
            });
        });
    }).get();
    BOOST_REQUIRE_EQUAL(expected , actual);
}

SEASTAR_THREAD_TEST_CASE(test_tmp_dir_with_non_existing_path) {
    BOOST_REQUIRE_EXCEPTION(tmp_dir::do_with("/tmp/this_name_should_not_exist", [] (tmp_dir&) {}).get(),
            std::system_error, testing::exception_predicate::message_contains("No such file or directory"));
}

SEASTAR_TEST_CASE(tmp_dir_with_thread_test) {
    return tmp_dir::do_with_thread([] (tmp_dir& td) {
        tmp_file tf = make_tmp_file(td.get_path()).get();
        auto& f = tf.get_file();
        auto buf = get_init_buffer(f);
        auto expected = buf.size();
        auto actual = f.dma_write(0, buf.get(), buf.size()).get();
        BOOST_REQUIRE_EQUAL(expected, actual);
        tf.close().get();
        tf.remove().get();
    });
}

SEASTAR_TEST_CASE(tmp_dir_with_leftovers_test) {
    return tmp_dir::do_with_thread([] (tmp_dir& td) {
        fs::path path = td.get_path() / "testfile.tmp";
        touch_file(path.native()).get();
        BOOST_REQUIRE(file_exists(path.native()).get());
    });
}

SEASTAR_TEST_CASE(tmp_dir_do_with_fail_func_test) {
    return tmp_dir::do_with_thread([] (tmp_dir& outer) {
        BOOST_REQUIRE_THROW(tmp_dir::do_with([] (tmp_dir& inner) mutable {
            return make_exception_future<>(expected_exception());
        }).get(), expected_exception);
    });
}

SEASTAR_TEST_CASE(tmp_dir_do_with_fail_remove_test) {
    return tmp_dir::do_with_thread([] (tmp_dir& outer) {
        auto saved_default_tmpdir = default_tmpdir();
        sstring outer_path = outer.get_path().native();
        sstring inner_path;
        sstring inner_path_renamed;
        set_default_tmpdir(outer_path.c_str());
        BOOST_REQUIRE_THROW(tmp_dir::do_with([&] (tmp_dir& inner) mutable {
            inner_path = inner.get_path().native();
            inner_path_renamed = inner_path + ".renamed";
            return rename_file(inner_path, inner_path_renamed);
        }).get(), std::system_error);
        BOOST_REQUIRE(!file_exists(inner_path).get());
        BOOST_REQUIRE(file_exists(inner_path_renamed).get());
        set_default_tmpdir(saved_default_tmpdir.c_str());
    });
}

SEASTAR_TEST_CASE(tmp_dir_do_with_thread_fail_func_test) {
    return tmp_dir::do_with_thread([] (tmp_dir& outer) {
        BOOST_REQUIRE_THROW(tmp_dir::do_with_thread([] (tmp_dir& inner) mutable {
            throw expected_exception();
        }).get(), expected_exception);
    });
}

SEASTAR_TEST_CASE(tmp_dir_do_with_thread_fail_remove_test) {
    return tmp_dir::do_with_thread([] (tmp_dir& outer) {
        auto saved_default_tmpdir = default_tmpdir();
        sstring outer_path = outer.get_path().native();
        sstring inner_path;
        sstring inner_path_renamed;
        set_default_tmpdir(outer_path.c_str());
        BOOST_REQUIRE_THROW(tmp_dir::do_with_thread([&] (tmp_dir& inner) mutable {
            inner_path = inner.get_path().native();
            inner_path_renamed = inner_path + ".renamed";
            return rename_file(inner_path, inner_path_renamed);
        }).get(), std::system_error);
        BOOST_REQUIRE(!file_exists(inner_path).get());
        BOOST_REQUIRE(file_exists(inner_path_renamed).get());
        set_default_tmpdir(saved_default_tmpdir.c_str());
    });
}

SEASTAR_TEST_CASE(test_read_entire_file_contiguous) {
    return tmp_file::do_with([] (tmp_file& tf) {
        return async([&tf] {
            file& f = tf.get_file();
            auto& eng = testing::local_random_engine;
            auto dist = std::uniform_int_distribution<unsigned>();
            size_t size = f.memory_dma_alignment() * (1 + dist(eng) % 1000);
            auto wbuf = temporary_buffer<char>::aligned(f.memory_dma_alignment(), size);
            for (size_t i = 0; i < size; i++) {
                static char chars[] = "abcdefghijklmnopqrstuvwxyz0123456789";
                wbuf.get_write()[i] = chars[dist(eng) % sizeof(chars)];
            }

            BOOST_REQUIRE_EQUAL(f.dma_write(0, wbuf.begin(), wbuf.size()).get(), wbuf.size());
            f.flush().get();

            sstring res = util::read_entire_file_contiguous(tf.get_path()).get();
            BOOST_REQUIRE_EQUAL(res, std::string_view(wbuf.begin(), wbuf.size()));
        });
    });
}
