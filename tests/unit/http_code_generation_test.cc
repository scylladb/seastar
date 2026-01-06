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

#define BOOST_TEST_MODULE http_code_generation

#include <boost/test/unit_test.hpp>
#include <seastar/http/typed_parameters.hh>
#include <seastar/http/request.hh>
#include <seastar/http/exception.hh>

using namespace seastar;
using namespace seastar::httpd;

// Simulated generated code from OpenAPI spec

namespace test_api {

namespace ns_get_user {
    /// Type-safe parameter accessor
    struct parameters {
        const http::request& _req;

        explicit parameters(const http::request& req) : _req(req) {}

        /// Get user_id parameter (required)
        int32_t user_id() const {
            return _req.get<int32_t>("user_id");
        }

        /// Get limit parameter (optional)
        std::optional<int32_t> limit() const {
            if (_req.has_query_param("limit") || _req.param.exists("limit")) {
                return _req.get<int32_t>("limit");
            }
            return std::nullopt;
        }

        /// Get enabled parameter (optional)
        std::optional<bool> enabled() const {
            if (_req.has_query_param("enabled") || _req.param.exists("enabled")) {
                return _req.get<bool>("enabled");
            }
            return std::nullopt;
        }
    };
}

}

BOOST_AUTO_TEST_CASE(test_generated_struct_required_param) {
    http::request req;
    req.set_query_param("user_id", "42");

    test_api::ns_get_user::parameters params(req);

    // Required parameter should return value directly
    BOOST_CHECK_EQUAL(params.user_id(), 42);
}

BOOST_AUTO_TEST_CASE(test_generated_struct_optional_param_present) {
    http::request req;
    req.set_query_param("user_id", "42");
    req.set_query_param("limit", "10");
    req.set_query_param("enabled", "true");

    test_api::ns_get_user::parameters params(req);

    // Optional parameters should return std::optional with values
    auto limit = params.limit();
    BOOST_REQUIRE(limit.has_value());
    BOOST_CHECK_EQUAL(*limit, 10);

    auto enabled = params.enabled();
    BOOST_REQUIRE(enabled.has_value());
    BOOST_CHECK_EQUAL(*enabled, true);
}

BOOST_AUTO_TEST_CASE(test_generated_struct_optional_param_missing) {
    http::request req;
    req.set_query_param("user_id", "42");

    test_api::ns_get_user::parameters params(req);

    // Optional parameters should return empty optional when missing
    auto limit = params.limit();
    BOOST_CHECK(!limit.has_value());

    auto enabled = params.enabled();
    BOOST_CHECK(!enabled.has_value());
}

BOOST_AUTO_TEST_CASE(test_generated_struct_required_param_missing) {
    http::request req;
    // user_id is required but not provided

    test_api::ns_get_user::parameters params(req);

    // Should throw missing_param_exception
    BOOST_CHECK_THROW(params.user_id(), missing_param_exception);
}

BOOST_AUTO_TEST_CASE(test_generated_struct_type_conversion_error) {
    http::request req;
    req.set_query_param("user_id", "not_a_number");

    test_api::ns_get_user::parameters params(req);

    // Should throw type_conversion_exception
    BOOST_CHECK_THROW(params.user_id(), type_conversion_exception);
}

BOOST_AUTO_TEST_CASE(test_generated_struct_with_validation) {
    http::request req;
    req.set_query_param("user_id", "42");
    req.set_query_param("limit", "150");

    // Set up metadata with constraints
    parameter_metadata_registry registry;
    registry.register_parameter(
        parameter_metadata("user_id", parameter_type::INT32, parameter_location::QUERY, true)
            .with_range(1, 1000)
    );
    registry.register_parameter(
        parameter_metadata("limit", parameter_type::INT32, parameter_location::QUERY, false)
            .with_range(1, 100)
    );

    req.set_parameter_metadata(&registry);

    test_api::ns_get_user::parameters params(req);

    // user_id is in valid range
    BOOST_CHECK_NO_THROW(params.user_id());
    BOOST_CHECK_EQUAL(params.user_id(), 42);

    // limit exceeds max (100)
    BOOST_CHECK_THROW(params.limit(), range_constraint_exception);
}

BOOST_AUTO_TEST_CASE(test_generated_struct_with_enum_validation) {
    // Test valid enum value
    {
        http::request req;
        req.set_query_param("user_id", "42");
        req.set_query_param("status", "active");

        // Set up metadata with enum constraint
        parameter_metadata_registry registry;
        registry.register_parameter(
            parameter_metadata("user_id", parameter_type::INT32, parameter_location::QUERY, true)
        );
        registry.register_parameter(
            parameter_metadata("status", parameter_type::STRING, parameter_location::QUERY, false)
                .with_enum({"active", "inactive", "pending"})
        );

        req.set_parameter_metadata(&registry);

        // Valid enum value
        BOOST_CHECK_NO_THROW(req.get<sstring>("status"));
        BOOST_CHECK_EQUAL(req.get<sstring>("status"), "active");
    }

    // Test invalid enum value (new request to avoid cache)
    {
        http::request req;
        req.set_query_param("user_id", "42");
        req.set_query_param("status", "unknown");

        parameter_metadata_registry registry;
        registry.register_parameter(
            parameter_metadata("user_id", parameter_type::INT32, parameter_location::QUERY, true)
        );
        registry.register_parameter(
            parameter_metadata("status", parameter_type::STRING, parameter_location::QUERY, false)
                .with_enum({"active", "inactive", "pending"})
        );

        req.set_parameter_metadata(&registry);

        // Invalid enum value should throw
        BOOST_CHECK_THROW(req.get<sstring>("status"), enum_constraint_exception);
    }
}
