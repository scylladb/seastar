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
 * Copyright (C) 2016 ScyllaDB.
 */
#include <vector>

#include <seastar/core/do_with.hh>
#include <seastar/testing/test_case.hh>
#include <seastar/core/sstring.hh>
#include <seastar/core/vector-data-sink.hh>
#include <seastar/json/formatter.hh>
#include <seastar/json/json_elements.hh>
#include <seastar/testing/thread_test_case.hh>

using namespace seastar;
using namespace json;

SEASTAR_TEST_CASE(test_simple_values) {
    BOOST_CHECK_EQUAL("3", formatter::to_json(3));
    BOOST_CHECK_EQUAL("3", formatter::to_json(3.0));
    BOOST_CHECK_EQUAL("3.5", formatter::to_json(3.5));
    BOOST_CHECK_EQUAL("true", formatter::to_json(true));
    BOOST_CHECK_EQUAL("false", formatter::to_json(false));

    BOOST_CHECK_EQUAL("\"apa\"", formatter::to_json("apa")); // to_json(const char*)
    BOOST_CHECK_EQUAL("\"apa\"", formatter::to_json(sstring("apa"))); // to_json(const sstring&)
    BOOST_CHECK_EQUAL("\"apa\"", formatter::to_json("apa", 3)); // to_json(const char*, size_t)

    using namespace std::string_literals;
    sstring str = "\0 COWA\bU\nGA [{\r}]\x1a"s,
            expected = "\"\\u0000 COWA\\bU\\nGA [{\\r}]\\u001A\""s;
    BOOST_CHECK_EQUAL(expected, formatter::to_json(str)); // to_json(const sstring&)
    BOOST_CHECK_EQUAL(expected, formatter::to_json(str.c_str(), str.size())); // to_json(const char*, size_t)

    return make_ready_future();
}

SEASTAR_TEST_CASE(test_collections) {
    BOOST_CHECK_EQUAL("{1:2,3:4}", formatter::to_json(std::map<int,int>({{1,2},{3,4}})));
    BOOST_CHECK_EQUAL("[1,2,3,4]", formatter::to_json(std::vector<int>({1,2,3,4})));
    BOOST_CHECK_EQUAL("[{1:2},{3:4}]", formatter::to_json(std::vector<std::pair<int,int>>({{1,2},{3,4}})));
    BOOST_CHECK_EQUAL("[{1:2},{3:4}]", formatter::to_json(std::vector<std::map<int,int>>({{{1,2}},{{3,4}}})));
    BOOST_CHECK_EQUAL("[[1,2],[3,4]]", formatter::to_json(std::vector<std::vector<int>>({{1,2},{3,4}})));

    return make_ready_future();
}

struct object_json : public json_base {
    json_element<sstring> subject;
    json_list<long> values;

    void register_params() {
      add(&subject, "subject");
      add(&values, "values");
    }

    object_json() { register_params(); }

    object_json(const object_json &e) {
      register_params();
      subject = e.subject;
      values = e.values;
    }
};

SEASTAR_TEST_CASE(test_jsonable) {
    object_json obj;
    obj.subject = "foo";
    obj.values.push(1);
    obj.values.push(2);
    obj.values.push(3);

    BOOST_CHECK_EQUAL("{\"subject\": \"foo\", \"values\": [1,2,3]}", formatter::to_json(obj));
    return make_ready_future();
}

SEASTAR_THREAD_TEST_CASE(test_stream_range_as_array) {
    auto vec = std::vector<net::packet>{};
    auto out = output_stream<char>(data_sink(std::make_unique<vector_data_sink>(vec)), 8);

    auto mapper = stream_range_as_array(std::vector<int>{1,2,3}, [] (auto i) {
        object_json obj;
        obj.subject = std::to_string(i);
        obj.values.push(i);
        return obj;
    });

    mapper(std::move(out)).get();

    auto packets = net::packet{};
    for (auto &p : vec) {
      packets.append(std::move(p));
    }
    packets.linearize();
    auto buf = packets.release();

    sstring result(buf.front().get(), buf.front().size());
    sstring expected = "[{\"subject\":\"1\",\"values\":[1]}, {\"subject\":\"2\",\"values\":[2]}, {\"subject\":\"3\",\"values\":[3]}]";
    BOOST_CHECK_EQUAL(expected, result);
}
