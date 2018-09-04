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
 * Copyright (C) 2017 ScyllaDB
 */

#define BOOST_TEST_MODULE core

#include <seastar/util/program-options.hh>

#include <boost/program_options.hpp>
#include <boost/test/included/unit_test.hpp>

#include <initializer_list>
#include <vector>

namespace bpo = boost::program_options;

using namespace seastar;

static bpo::variables_map parse(const bpo::options_description& desc, std::initializer_list<const char*> args) {
    std::vector<const char*> raw_args{"program_options_test"};
    for (const char* arg : args) {
        raw_args.push_back(arg);
    }

    bpo::variables_map vars;
    bpo::store(bpo::parse_command_line(raw_args.size(), raw_args.data(), desc), vars);
    bpo::notify(vars);

    return vars;
}

BOOST_AUTO_TEST_CASE(string_map) {
    bpo::options_description desc;
    desc.add_options()
            ("ages", bpo::value<program_options::string_map>());

    const auto vars = parse(desc, {"--ages", "joe=15:sally=20", "--ages", "phil=18:joe=11"});
    const auto& ages = vars["ages"].as<program_options::string_map>();

    // `string_map` values can be specified multiple times. The last association takes precedence.
    BOOST_REQUIRE_EQUAL(ages.at("joe"), "11");
    BOOST_REQUIRE_EQUAL(ages.at("phil"), "18");
    BOOST_REQUIRE_EQUAL(ages.at("sally"), "20");

    BOOST_REQUIRE_THROW(parse(desc, {"--ages", "tim:"}), bpo::invalid_option_value);
}
