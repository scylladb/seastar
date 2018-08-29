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
 * Copyright (C) 2018 ScyllaDB Ltd.
 */

#define BOOST_TEST_MODULE simple_stream

#include <boost/test/included/unit_test.hpp>
#include "core/simple-stream.hh"

using namespace seastar;

template<typename Input, typename Output>
static void write_read_test(Input in, Output out)
{
    auto aa = std::vector<char>(4, 'a');
    auto bb = std::vector<char>(3, 'b');
    auto cc = std::vector<char>(2, 'c');

    out.write(aa.data(), aa.size());
    out.fill('b', 3);

    BOOST_CHECK_THROW(out.fill(' ', 3), std::out_of_range);
    BOOST_CHECK_THROW(out.write("   ", 3), std::out_of_range);

    out.write(cc.data(), cc.size());

    BOOST_CHECK_THROW(out.fill(' ', 1), std::out_of_range);
    BOOST_CHECK_THROW(out.write(" ", 1), std::out_of_range);

    auto actual_aa = std::vector<char>(4);
    in.read(actual_aa.data(), actual_aa.size());
    BOOST_CHECK_EQUAL(aa, actual_aa);

    auto actual_bb = std::vector<char>(3);
    in.read(actual_bb.data(), actual_bb.size());
    BOOST_CHECK_EQUAL(bb, actual_bb);

    actual_aa.resize(1024);
    BOOST_CHECK_THROW(in.read(actual_aa.data(), actual_aa.size()), std::out_of_range);

    auto actual_cc = std::vector<char>(2);
    in.read(actual_cc.data(), actual_cc.size());
    BOOST_CHECK_EQUAL(cc, actual_cc);

    BOOST_CHECK_THROW(in.read(actual_aa.data(), 1), std::out_of_range);
}

BOOST_AUTO_TEST_CASE(simple_write_read_test) {
    auto buf = temporary_buffer<char>(9);

    write_read_test(simple_memory_input_stream(buf.get(), buf.size()),
                    simple_memory_output_stream(buf.get_write(), buf.size()));

    std::fill_n(buf.get_write(), buf.size(), 'x');

    auto out = simple_memory_output_stream(buf.get_write(), buf.size());
    write_read_test(out.to_input_stream(), out);
}

BOOST_AUTO_TEST_CASE(fragmented_write_read_test) {
    static constexpr size_t total_size = 9;

    auto bufs = std::vector<temporary_buffer<char>>();
    using iterator_type = std::vector<temporary_buffer<char>>::iterator;

    auto test = [&] {
        write_read_test(fragmented_memory_input_stream<iterator_type>(bufs.begin(), total_size),
                        fragmented_memory_output_stream<iterator_type>(bufs.begin(), total_size));

        auto out = fragmented_memory_output_stream<iterator_type>(bufs.begin(), total_size);
        write_read_test(out.to_input_stream(), out);
    };

    bufs.emplace_back(total_size);
    test();

    bufs.clear();
    for (auto i = 0u; i < total_size; i++) {
        bufs.emplace_back(1);
    }
    test();
}
