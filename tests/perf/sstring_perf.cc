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
 * Copyright (C) 2023 ScyllaDB Ltd.
 */

#include <algorithm>
#include <climits>
#include <random>

#include <seastar/testing/perf_tests.hh>
#include <seastar/core/sstring.hh>

class random_bytes {
protected:
    // sometimes, we use basic_sstring<int8_t> to represent a byte array
    using char_type = int8_t;
    using string_t = basic_sstring<char_type, uint32_t, 31, false>;
    string_t _s1 = random_string();
    string_t _s2 = random_string();
    // assuming it's the typical length of a byte string
    static constexpr size_t size = 30;
private:
    static string_t random_string() {
        // generate the data with random bits, so compiler does not optimize
        // the comparision with constant propagation.
        using random_bytes_engine =
            std::independent_bits_engine<std::default_random_engine,
                                         CHAR_BIT,
                                         unsigned char>;
        random_bytes_engine rng;
        rng.seed(time(nullptr));
        string_t s{string_t::initialized_later{}, size};
        std::generate(std::begin(s), std::end(s), std::ref(rng));
        return s;
    }
public:
    random_bytes() = default;
};

PERF_TEST_F(random_bytes, compare) {
    auto r = _s1.compare(_s2);
    perf_tests::do_not_optimize(r);
}
