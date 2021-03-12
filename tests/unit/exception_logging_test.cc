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
 * Copyright 2016 ScyllaDB
 */

#define BOOST_TEST_MODULE core

#include <boost/test/included/unit_test.hpp>
#include <seastar/util/log.hh>
#include <ostream>


using namespace seastar;

// a class which is not derived from std::exception
// to play the part of the unknown object in the logging
// function.
class unknown_obj {
    sstring _message;
public:
    unknown_obj(std::string message) : _message(message) {}
};

// This functions generates an exception chain nesting_level+1 deep
// for each nesting level it throws one of two types of objects, an
// unknown (non std::exception) object or a runtime error which is
// derived from std::exception, it chooses the type of thrown object
// according to the bit in the `nesting_level` place in the
// `tests_instance` paramter or in other words according to:
// bool(test_instance & (1<<nesting_level))
void exception_generator(uint32_t test_instance, int nesting_level) {
    try {
        if (nesting_level > 0) {
            exception_generator(test_instance>>1, nesting_level-1);
        }
    } catch(...) {
        auto msg = format("Exception Level {}", nesting_level);
        if(test_instance&1) {
            // Throw a non std::exception derived type
            std::throw_with_nested(unknown_obj(msg));
        } else {
            std::throw_with_nested(std::runtime_error(msg));
        }
    }
    if (nesting_level == 0) {
        if (test_instance & 1) {
            throw unknown_obj(format("Exception Level {}", nesting_level));
        } else {
            throw std::runtime_error(format("Exception Level {}", nesting_level));
        }
    }
}

// This function generates the expected logging output string of an exception
// thrown by the  exception generator function with a specific output.
std::string exception_generator_str(uint32_t test_instance,int nesting_level) {
    std::ostringstream ret;
    const std::string runtime_err_str = "std::runtime_error";
    const std::string exception_level_fmt_str = "Exception Level {}";
    const std::string unknown_obj_str = "unknown_obj";
    const std::string nested_exception_with_unknown_obj_str = "std::_Nested_exception<unknown_obj>";
    const std::string nested_exception_with_runtime_err_str = "std::_Nested_exception<std::runtime_error>";

    for(; nesting_level > 0; nesting_level--) {
        if (test_instance & 1) {
            ret << nested_exception_with_unknown_obj_str;
        } else {
            ret << nested_exception_with_runtime_err_str << " (" <<
                    format(exception_level_fmt_str.c_str(), nesting_level) << ")";
        }
        ret << ": ";
        test_instance >>= 1;
    }


    if (test_instance & 1) {
        ret << unknown_obj_str;
    } else {
        ret << runtime_err_str << " (" << format(exception_level_fmt_str.c_str(), nesting_level) << ")";
    }
    return ret.str();
}

// Test all variations of nested exceptions of some
// depth
BOOST_AUTO_TEST_CASE(nested_exception_logging1) {

    constexpr int levels_to_test = 3;

    for(int level = 0; level < levels_to_test; level++) {
        for(int inst = (1 << (level + 1)) - 1; inst >= 0; inst--) {
            std::ostringstream log_msg;
            try {
                exception_generator(inst, level);
            } catch(...) {
                log_msg << std::current_exception();
            }
            BOOST_REQUIRE_EQUAL(log_msg.str(), exception_generator_str(inst, level));
        }
    }
}

// Test logging of nested exception not mixed in with anything
BOOST_AUTO_TEST_CASE(nested_exception_logging2) {
    std::ostringstream log_msg;
    try {
        throw std::nested_exception();
    } catch(...) {
        log_msg << std::current_exception();
    }

    BOOST_REQUIRE_EQUAL(log_msg.str(), std::string("std::nested_exception: <no exception>"));
}

BOOST_AUTO_TEST_CASE(unknown_object_thrown_test) {
    std::ostringstream log_msg;
    try {
        throw unknown_obj("This is an unknown object");
    } catch(...) {
        log_msg << std::current_exception();
    }

    BOOST_REQUIRE_EQUAL(log_msg.str(), std::string("unknown_obj"));

}