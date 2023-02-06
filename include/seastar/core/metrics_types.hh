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
 * Copyright (C) 2016 ScyllaDB
 */

#pragma once

#include <cstdint>
#include <vector>

namespace seastar {
namespace metrics {


/*!
 * \brief Histogram bucket type
 *
 * A histogram bucket contains an upper bound and the number
 * of events in the buckets.
 */
struct histogram_bucket {
    uint64_t count = 0; // number of events.
    double upper_bound = 0;      // Inclusive.
};


/*!
 * \brief Histogram data type
 *
 * The histogram struct is a container for histogram values.
 * It is not a histogram implementation but it will be used by histogram
 * implementation to return its internal values.
 */
struct histogram {
    uint64_t sample_count = 0;
    double sample_sum = 0;
    std::vector<histogram_bucket> buckets; // Ordered in increasing order of upper_bound, +Inf bucket is optional.

    /*!
     * \brief Addition assigning a historgram
     *
     * The histogram must match the buckets upper bounds
     * or an exception will be thrown
     */
    histogram& operator+=(const histogram& h);

    /*!
     * \brief Addition historgrams
     *
     * Add two histograms and return the result as a new histogram
     * The histogram must match the buckets upper bounds
     * or an exception will be thrown
     */
    histogram operator+(const histogram& h) const;

    /*!
     * \brief Addition historgrams
     *
     * Add two histograms and return the result as a new histogram
     * The histogram must match the buckets upper bounds
     * or an exception will be thrown
     */
    histogram operator+(histogram&& h) const;

};

}

}
