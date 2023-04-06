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
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 */

#include <seastar/core/app-template.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/core/vector-data-sink.hh>
#include <seastar/core/loop.hh>
#include <seastar/util/later.hh>
#include <seastar/core/sstring.hh>
#include <seastar/net/packet.hh>
#include <seastar/testing/test_case.hh>
#include <seastar/testing/thread_test_case.hh>
#include <vector>

using namespace seastar;
using namespace net;

static sstring to_sstring(const packet& p) {
    sstring res = uninitialized_string(p.len());
    auto i = res.begin();
    for (auto& frag : p.fragments()) {
        i = std::copy(frag.base, frag.base + frag.size, i);
    }
    return res;
}

struct stream_maker {
    output_stream_options opts;
    size_t _size;

    stream_maker size(size_t size) && {
        _size = size;
        return std::move(*this);
    }

    stream_maker trim(bool do_trim) && {
        opts.trim_to_size = do_trim;
        return std::move(*this);
    }

    lw_shared_ptr<output_stream<char>> operator()(data_sink sink) {
        return make_lw_shared<output_stream<char>>(std::move(sink), _size, opts);
    }
};

template <typename T, typename StreamConstructor>
future<> assert_split(StreamConstructor stream_maker, std::initializer_list<T> write_calls,
        std::vector<std::string> expected_split) {
    static int i = 0;
    BOOST_TEST_MESSAGE("checking split: " << i++);
    auto sh_write_calls = make_lw_shared<std::vector<T>>(std::move(write_calls));
    auto sh_expected_splits = make_lw_shared<std::vector<std::string>>(std::move(expected_split));
    auto v = make_shared<std::vector<packet>>();
    auto out = stream_maker(data_sink(std::make_unique<vector_data_sink>(*v)));

    return do_for_each(sh_write_calls->begin(), sh_write_calls->end(), [out, sh_write_calls] (auto&& chunk) {
        return out->write(chunk);
    }).then([out, v, sh_expected_splits] {
        return out->close().then([out, v, sh_expected_splits] {
            BOOST_REQUIRE_EQUAL(v->size(), sh_expected_splits->size());
            int i = 0;
            for (auto&& chunk : *sh_expected_splits) {
                BOOST_REQUIRE(to_sstring((*v)[i]) == chunk);
                i++;
            }
        });
    });
}

SEASTAR_TEST_CASE(test_splitting) {
    auto ctor = stream_maker().trim(false).size(4);
    return now()
        .then([=] { return assert_split(ctor, {"1"}, {"1"}); })
        .then([=] { return assert_split(ctor, {"12", "3"}, {"123"}); })
        .then([=] { return assert_split(ctor, {"12", "34"}, {"1234"}); })
        .then([=] { return assert_split(ctor, {"12", "345"}, {"1234", "5"}); })
        .then([=] { return assert_split(ctor, {"1234"}, {"1234"}); })
        .then([=] { return assert_split(ctor, {"12345"}, {"12345"}); })
        .then([=] { return assert_split(ctor, {"1234567890"}, {"1234567890"}); })
        .then([=] { return assert_split(ctor, {"1", "23456"}, {"1234", "56"}); })
        .then([=] { return assert_split(ctor, {"123", "4567"}, {"1234", "567"}); })
        .then([=] { return assert_split(ctor, {"123", "45678"}, {"1234", "5678"}); })
        .then([=] { return assert_split(ctor, {"123", "4567890"}, {"1234", "567890"}); })
        .then([=] { return assert_split(ctor, {"1234", "567"}, {"1234", "567"}); })

        .then([] { return assert_split(stream_maker().trim(false).size(3), {"1", "234567", "89"}, {"123", "4567", "89"}); })
        .then([] { return assert_split(stream_maker().trim(false).size(3), {"1", "2345", "67"}, {"123", "456", "7"}); })
        ;
}

SEASTAR_TEST_CASE(test_splitting_with_trimming) {
    auto ctor = stream_maker().trim(true).size(4);
    return now()
        .then([=] { return assert_split(ctor, {"1"}, {"1"}); })
        .then([=] { return assert_split(ctor, {"12", "3"}, {"123"}); })
        .then([=] { return assert_split(ctor, {"12", "3456789"}, {"1234", "5678", "9"}); })
        .then([=] { return assert_split(ctor, {"12", "3456789", "12"}, {"1234", "5678", "912"}); })
        .then([=] { return assert_split(ctor, {"123456789"}, {"1234", "5678", "9"}); })
        .then([=] { return assert_split(ctor, {"12345678"}, {"1234", "5678"}); })
        .then([=] { return assert_split(ctor, {"12345678", "9"}, {"1234", "5678", "9"}); })
        .then([=] { return assert_split(ctor, {"1234", "567890"}, {"1234", "5678", "90"}); })
        ;
}

SEASTAR_TEST_CASE(test_flush_on_empty_buffer_does_not_push_empty_packet_down_stream) {
    auto v = make_shared<std::vector<packet>>();
    auto out = make_shared<output_stream<char>>(
        data_sink(std::make_unique<vector_data_sink>(*v)), 8);

    return out->flush().then([v, out] {
        BOOST_REQUIRE(v->empty());
        return out->close();
    }).finally([out]{});
}

SEASTAR_THREAD_TEST_CASE(test_simple_write) {
    auto vec = std::vector<net::packet>{};
    auto out = output_stream<char>(data_sink(std::make_unique<vector_data_sink>(vec)), 8);

    auto value1 = sstring("te");
    out.write(value1).get();


    auto value2 = sstring("st");
    out.write(value2).get();

    auto value3 = sstring("abcdefgh1234");
    out.write(value3).get();

    out.close().get();

    auto value = value1 + value2 + value3;
    auto packets = net::packet{};
    for (auto& p : vec) {
        packets.append(std::move(p));
    }
    packets.linearize();
    auto buf = packets.release();
    BOOST_REQUIRE_EQUAL(buf.size(), 1);
    BOOST_REQUIRE_EQUAL(sstring(buf.front().get(), buf.front().size()), value);
}

class data_sink_assertions final : public data_sink_impl {
    size_t _total_bytes = 0;
    size_t _expected_bytes;
    bool _flushed = false;
    bool _closed = false;
    std::exception_ptr _flush_ex;
public:
    data_sink_assertions(size_t total_bytes, std::exception_ptr flush_ex = nullptr) : _expected_bytes(total_bytes), _flush_ex(std::move(flush_ex)) {}

    virtual future<> put(net::packet p) override {
        assert(false);
        return make_ready_future<>();
    }

    virtual future<> put(temporary_buffer<char> buf) {
        _total_bytes += buf.size();
        return make_ready_future<>();
    }

    virtual future<> put(std::vector<temporary_buffer<char>> data) {
        assert(false);
        return make_ready_future<>();
    }

    virtual future<> flush() override {
        if (_flush_ex) {
            return make_exception_future<>(_flush_ex);
        }
        _flushed = true;
        return make_ready_future<>();
    }

    virtual future<> close() override {
        _closed = true;
        return make_ready_future<>();
    }

    virtual ~data_sink_assertions() {
        BOOST_REQUIRE_EQUAL(_total_bytes, _expected_bytes);
        if (!_flush_ex) {
            BOOST_REQUIRE(_flushed);
        }
        BOOST_REQUIRE(_closed);
    }
};

SEASTAR_THREAD_TEST_CASE(test_with_output_stream_norm) {
    auto out = output_stream<char>(data_sink(std::make_unique<data_sink_assertions>(4)), 8);
    with_output_stream(std::move(out), [] (output_stream<char>& out) -> future<int> {
        return async([&out] {
            out.write("1").get();
            out.write("2").get();
            out.write("3").get();
            out.write("4").get();
        }).then([] {
            return make_ready_future<int>(42);
        });
    }).then([] (int res) {
        BOOST_REQUIRE_EQUAL(res, 42);
    }).get();
}

SEASTAR_THREAD_TEST_CASE(test_with_output_stream_throw_func) {
    struct func_exception : public std::exception {};
    auto out = output_stream<char>(data_sink(std::make_unique<data_sink_assertions>(0)), 8);
    with_output_stream(std::move(out), [] (output_stream<char>& out) {
        return make_exception_future<>(func_exception{});
    }).then_wrapped([] (auto fut) {
        BOOST_REQUIRE_THROW(fut.get(), func_exception);
    }).get();
}

SEASTAR_THREAD_TEST_CASE(test_with_output_stream_throw_flush) {
    struct flush_exception : public std::exception {};
    auto out = output_stream<char>(data_sink(std::make_unique<data_sink_assertions>(1, make_exception_ptr(flush_exception{}))), 8);
    with_output_stream(std::move(out), [] (output_stream<char>& out) {
        return out.write("a");
    }).then_wrapped([] (auto fut) {
        BOOST_REQUIRE_THROW(fut.get(), flush_exception);
    }).get();
}

SEASTAR_THREAD_TEST_CASE(test_with_output_stream_throw_func_and_flush) {
    // In this case func_ and flush_ exceptions would nest
    struct func_exception : public std::exception {};
    struct flush_exception : public std::exception {};
    auto out = output_stream<char>(data_sink(std::make_unique<data_sink_assertions>(0, make_exception_ptr(flush_exception{}))), 8);
    with_output_stream(std::move(out), [] (output_stream<char>& out) {
        return make_exception_future<>(func_exception{});
    }).then_wrapped([] (auto fut) {
        try {
            fut.get();
            BOOST_REQUIRE(false);
        } catch (seastar::nested_exception& ex) {
            BOOST_REQUIRE_THROW(std::rethrow_exception(ex.inner), flush_exception);
            BOOST_REQUIRE_THROW(ex.rethrow_nested(), func_exception);
        } catch (...) {
            BOOST_REQUIRE(false);
        }
    }).get();
}
