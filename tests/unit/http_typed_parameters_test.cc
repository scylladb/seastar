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
 * Copyright (C) 2025 Kefu Chai (tchaikov@gmail.com)
 */

#define BOOST_TEST_MODULE http_typed_parameters

#include <boost/test/unit_test.hpp>
#include <seastar/http/typed_parameters.hh>
#include <seastar/http/request.hh>
#include <seastar/http/exception.hh>

using namespace seastar;
using namespace seastar::httpd;

BOOST_AUTO_TEST_CASE(test_parameter_converter_int32) {
    // Valid conversions
    BOOST_CHECK_EQUAL(parameter_converter<int32_t>::convert("123"), 123);
    BOOST_CHECK_EQUAL(parameter_converter<int32_t>::convert("-456"), -456);
    BOOST_CHECK_EQUAL(parameter_converter<int32_t>::convert("0"), 0);

    // Invalid conversions
    BOOST_CHECK_THROW(parameter_converter<int32_t>::convert("abc"), type_conversion_exception);
    BOOST_CHECK_THROW(parameter_converter<int32_t>::convert("123.45"), type_conversion_exception);
    BOOST_CHECK_THROW(parameter_converter<int32_t>::convert(""), type_conversion_exception);
    BOOST_CHECK_THROW(parameter_converter<int32_t>::convert("999999999999999"), type_conversion_exception);
}

BOOST_AUTO_TEST_CASE(test_parameter_converter_int64) {
    // Valid conversions
    BOOST_CHECK_EQUAL(parameter_converter<int64_t>::convert("123456789012345"), 123456789012345LL);
    BOOST_CHECK_EQUAL(parameter_converter<int64_t>::convert("-987654321"), -987654321LL);

    // Invalid conversions
    BOOST_CHECK_THROW(parameter_converter<int64_t>::convert("not_a_number"), type_conversion_exception);
}

BOOST_AUTO_TEST_CASE(test_parameter_converter_double) {
    // Valid conversions
    BOOST_CHECK_CLOSE(parameter_converter<double>::convert("123.45"), 123.45, 0.001);
    BOOST_CHECK_CLOSE(parameter_converter<double>::convert("-0.001"), -0.001, 0.00001);
    BOOST_CHECK_EQUAL(parameter_converter<double>::convert("123"), 123.0);

    // Invalid conversions
    BOOST_CHECK_THROW(parameter_converter<double>::convert("abc"), type_conversion_exception);
}

BOOST_AUTO_TEST_CASE(test_parameter_converter_bool) {
    // True values
    BOOST_CHECK(parameter_converter<bool>::convert("true"));
    BOOST_CHECK(parameter_converter<bool>::convert("True"));
    BOOST_CHECK(parameter_converter<bool>::convert("TRUE"));
    BOOST_CHECK(parameter_converter<bool>::convert("1"));
    BOOST_CHECK(parameter_converter<bool>::convert("yes"));
    BOOST_CHECK(parameter_converter<bool>::convert("Yes"));
    BOOST_CHECK(parameter_converter<bool>::convert("YES"));

    // False values
    BOOST_CHECK(!parameter_converter<bool>::convert("false"));
    BOOST_CHECK(!parameter_converter<bool>::convert("False"));
    BOOST_CHECK(!parameter_converter<bool>::convert("FALSE"));
    BOOST_CHECK(!parameter_converter<bool>::convert("0"));
    BOOST_CHECK(!parameter_converter<bool>::convert("no"));
    BOOST_CHECK(!parameter_converter<bool>::convert("No"));
    BOOST_CHECK(!parameter_converter<bool>::convert("NO"));

    // Invalid values
    BOOST_CHECK_THROW(parameter_converter<bool>::convert("maybe"), type_conversion_exception);
    BOOST_CHECK_THROW(parameter_converter<bool>::convert("2"), type_conversion_exception);
}

BOOST_AUTO_TEST_CASE(test_parameter_converter_string) {
    // Strings are passthrough
    BOOST_CHECK_EQUAL(parameter_converter<sstring>::convert("hello"), "hello");
    BOOST_CHECK_EQUAL(parameter_converter<sstring>::convert(""), "");
    BOOST_CHECK_EQUAL(parameter_converter<sstring>::convert("123"), "123");
}

BOOST_AUTO_TEST_CASE(test_request_typed_get_with_default) {
    http::request req;
    req.set_query_param("id", "42");
    req.set_query_param("name", "test");
    req.set_query_param("active", "true");

    // Get with correct types
    BOOST_CHECK_EQUAL(req.get<int32_t>("id", 0), 42);
    BOOST_CHECK_EQUAL(req.get<sstring>("name", "default"), "test");
    BOOST_CHECK_EQUAL(req.get<bool>("active", false), true);

    // Get missing with default
    BOOST_CHECK_EQUAL(req.get<int32_t>("missing", 99), 99);
    BOOST_CHECK_EQUAL(req.get<sstring>("missing", "default"), "default");
}

BOOST_AUTO_TEST_CASE(test_request_typed_get_required) {
    http::request req;
    req.set_query_param("id", "42");
    req.set_query_param("invalid_int", "not_a_number");

    // Get existing parameter
    BOOST_CHECK_EQUAL(req.get<int32_t>("id"), 42);

    // Get missing parameter - should throw
    BOOST_CHECK_THROW(req.get<int32_t>("missing"), missing_param_exception);

    // Get parameter with invalid conversion - should throw
    BOOST_CHECK_THROW(req.get<int32_t>("invalid_int"), type_conversion_exception);
}

BOOST_AUTO_TEST_CASE(test_range_constraint) {
    parameter_metadata meta("age", parameter_type::INT32, parameter_location::QUERY, true);
    meta.with_range(18, 100);

    // Valid values
    BOOST_CHECK_NO_THROW(meta.validate("50"));
    BOOST_CHECK_NO_THROW(meta.validate("18"));
    BOOST_CHECK_NO_THROW(meta.validate("100"));

    // Invalid values
    BOOST_CHECK_THROW(meta.validate("17"), range_constraint_exception);
    BOOST_CHECK_THROW(meta.validate("101"), range_constraint_exception);
    BOOST_CHECK_THROW(meta.validate("-5"), range_constraint_exception);
}

BOOST_AUTO_TEST_CASE(test_length_constraint) {
    parameter_metadata meta("username", parameter_type::STRING, parameter_location::QUERY, true);
    meta.with_length(3, 20);

    // Valid values
    BOOST_CHECK_NO_THROW(meta.validate("abc"));
    BOOST_CHECK_NO_THROW(meta.validate("abcdefghij"));
    BOOST_CHECK_NO_THROW(meta.validate("12345678901234567890"));

    // Invalid values
    BOOST_CHECK_THROW(meta.validate("ab"), length_constraint_exception);
    BOOST_CHECK_THROW(meta.validate("123456789012345678901"), length_constraint_exception);
    BOOST_CHECK_THROW(meta.validate(""), length_constraint_exception);
}

BOOST_AUTO_TEST_CASE(test_enum_constraint) {
    parameter_metadata meta("status", parameter_type::STRING, parameter_location::QUERY, true);
    meta.with_enum({"active", "inactive", "pending"});

    // Valid values
    BOOST_CHECK_NO_THROW(meta.validate("active"));
    BOOST_CHECK_NO_THROW(meta.validate("inactive"));
    BOOST_CHECK_NO_THROW(meta.validate("pending"));

    // Invalid values
    BOOST_CHECK_THROW(meta.validate("unknown"), enum_constraint_exception);
    BOOST_CHECK_THROW(meta.validate("Active"), enum_constraint_exception);
}

BOOST_AUTO_TEST_CASE(test_metadata_registry) {
    parameter_metadata_registry registry;

    // Add metadata
    registry.register_parameter(
        parameter_metadata("id", parameter_type::INT32, parameter_location::QUERY, true)
            .with_range(1, 1000)
    );
    registry.register_parameter(
        parameter_metadata("name", parameter_type::STRING, parameter_location::QUERY, false)
            .with_length(1, 50)
    );

    // Retrieve metadata
    auto* id_meta = registry.get_metadata("id");
    BOOST_REQUIRE(id_meta != nullptr);
    BOOST_CHECK_EQUAL(id_meta->name(), "id");
    BOOST_CHECK(id_meta->required());

    auto* name_meta = registry.get_metadata("name");
    BOOST_REQUIRE(name_meta != nullptr);
    BOOST_CHECK_EQUAL(name_meta->name(), "name");
    BOOST_CHECK(!name_meta->required());

    // Non-existent parameter
    auto* missing_meta = registry.get_metadata("missing");
    BOOST_CHECK(missing_meta == nullptr);
}
