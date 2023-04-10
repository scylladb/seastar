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

#ifndef SEASTAR_MODULE
#include <cstdint>
#include <vector>
#include <seastar/util/modules.hh>
#include <optional>
#endif

namespace seastar {
namespace metrics {

SEASTAR_MODULE_EXPORT_BEGIN

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
 * \brief native histogram specific information
 *
 * Native histograms (also known as sparse histograms) are an experimental Prometheus feature.
 */
struct native_histogram_info {
    // schema defines the bucket schema. Currently, valid numbers are -4 <= n <= 8.
    // They are all for base-2 bucket schemas, where 1 is a bucket boundary in each case, and
    // then each power of two is divided into 2^n logarithmic buckets.
    // Or in other words, each bucket boundary is the previous boundary times 2^(2^-n).
    // In the future, more bucket schemas may be added using numbers < -4 or > 8.
    int32_t schema;
    // min_id is the first bucket id of a given schema.
    int32_t min_id;
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

    // Native histograms are an experimental Prometheus feature.
    std::optional<native_histogram_info> native_histogram;
};

SEASTAR_MODULE_EXPORT_END

}

}
