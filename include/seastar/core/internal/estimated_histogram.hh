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
 * Copyright (C) 2023 ScyllaDB.
 */

#pragma once

#include <cmath>
#include <algorithm>
#include <vector>
#include <chrono>
#include <string>
#include <seastar/core/metrics_types.hh>
#include <seastar/core/bitops.hh>
#include <limits>
#include <array>

namespace seastar::metrics::internal {
/**
 * This is a pseudo-exponential implementation of an estimated histogram.
 *
 * An exponential-histogram with coefficient 'coef', is a histogram where for bucket 'i'
 * the lower limit is coef^i and the higher limit is coef^(i+1).
 *
 * A pseudo-exponential is similar but the bucket limits are an approximation.
 *
 * The approximate_exponential_histogram is an efficient pseudo-exponential implementation.
 *
 * The histogram is defined by a Min and Max value limits, and a Precision (all should be power of 2
 * and will be explained).
 *
 * When adding a value to a histogram:
 * All values lower than Min will be included in the first bucket (the assumption is that it's
 * not suppose to happen but it is ok if it does).
 *
 * All values higher than Max will be included in the last bucket that serves as the
 * infinity bucket (the assumption is that it can happen but it is rare).
 *
 * Note the difference between the first and last buckets.
 * The first bucket is just like a regular bucket but has a second roll to collect unexpected low values.
 * The last bucket, also known as the infinity bucket, collect all values that passes the defined Max,
 * it only collect those values.
 *
 * Buckets Distribution (limits)
 * =============================
 * The buckets limits in the histogram are defined similar to a floating-point representation.
 *
 * Buckets limits have an exponent part and a linear part.
 *
 * The exponential part is a power of 2. Each power-of-2 range [2^n..2^n+1)
 * is split linearly to 'Precision' number of buckets.
 *
 * The total number of buckets is:
 *   NUM_BUCKETS = log2(Max/Min)*Precision +1
 *
 * For example, if the Min value is 128, the Max is 1024 and the Precision is 4, the number of buckets is 13.
 *
 * Anything below 160 will be in the bucket 0, anything above 1024 will be in bucket 13.
 * Note that the first bucket will include all values below Min.
 *
 * the range [128, 1024) will be split into log2(1024/128) = 3 ranges:
 * 128, 256, 512, 1024
 * Or more mathematically: [128, 256), [256, 512), [512,1024)
 *
 * Each range is split into 4 (The Precision).
 * 128            | 256            | 512            | 1024
 * 128 160 192 224| 256 320 384 448| 512 640 768 896|
 *
 * To get the exponential part of an index you divide by the Precision.
 * The linear part of the index is Modulus the precision.
 *
 * Calculating the bucket lower limit of bucket i:
 * The exponential part: exp_part = 2^floor(i/Precision)* Min
 *    with the above example 2^floor(i/4)*128
 * The linear part: (i%Precision) * (exp_part/Precision)
 *    With the example: (i%4) * (exp_part/4)
 *
 * So the lower limit of bucket 6:
 *    2^floor(6/4)* 128  = 256
 *    (6%4) * 256/4 = 128
 *    lower-limit   = 384
 *
 * How to find a bucket index for a value
 * =======================================
 * The bucket index consist of two parts:
 * higher bits (exponential part) are based on log2(value/min)
 *
 * lower bits (linear part) are based on the 'n' MSB (ignoring the leading 1) where n=log2(Precision).
 * Continuing with the example where the number of precision bits: PRECISION_BITS = log2(4) = 2
 *
 * for example: 330 (101001010)
 * The number of precision_bits: PRECISION_BITS = log2(4) = 2
 * higher bits: log2(330/128) = 1
 * MSB: 01 (the highest two bits following the leading 1)
 * So the index: 101 = 5
 *
 * About the Min, Max and Precision
 * ================================
 * For Min, Max and Precision, choose numbers that are a power of 2.
 *
 * Limitation: You must set the MIN value to be higher or equal to the Precision.
 *
 * \param Min - The lowest expected value. A value below that will be set to the Min value.
 * \param Max - The highest number to consider. A value above that is considered infinite.
 * \param Precision - The number of buckets in each range (where a range is the i^2..i^(2+1) )
 */
template<uint64_t Min, uint64_t Max, size_t Precision>
class approximate_exponential_histogram {
    static constexpr unsigned NUM_EXP_RANGES = log2floor(Max/Min);
    static constexpr size_t NUM_BUCKETS = NUM_EXP_RANGES * Precision + 1;
    static constexpr unsigned PRECISION_BITS = log2floor(Precision);
    static constexpr unsigned BASESHIFT = log2floor(Min);
    static constexpr uint64_t LOWER_BITS_MASK = Precision - 1;
    static constexpr size_t MIN_ID = log2ceil(Min) * Precision + 1; // id0 (x,1]
    std::array<uint64_t, NUM_BUCKETS> _buckets;
public:
    approximate_exponential_histogram() {
        clear();
    }

    /*!
     * \brief Returns the bucket lower limit given the bucket id.
     * The first and last bucket will always return the MIN and MAX respectively.
     *
     */
    uint64_t get_bucket_lower_limit(uint16_t bucket_id) const {
        if (bucket_id == NUM_BUCKETS - 1) {
            return Max;
        }
        int16_t exp_rang = (bucket_id >> PRECISION_BITS);
        return (Min << exp_rang) +  ((bucket_id & LOWER_BITS_MASK) << (exp_rang + BASESHIFT - PRECISION_BITS));
    }

    /*!
     * \brief Returns the bucket upper limit given the bucket id.
     * The last bucket (Infinity bucket) will return UMAX_INT.
     *
     */
    uint64_t get_bucket_upper_limit(uint16_t bucket_id) const {
        if (bucket_id == NUM_BUCKETS - 1) {
            return std::numeric_limits<uint64_t>::max();
        }
        return get_bucket_lower_limit(bucket_id + 1);
    }

    /*!
     * \brief Find the bucket index for a given value
     * The position of a value that is lower or equal to Min will always be 0.
     * The position of a value that is higher or equal to MAX will always be NUM_BUCKETS - 1.
     */
    uint16_t find_bucket_index(uint64_t val) const {
        if (val >= Max) {
            return NUM_BUCKETS - 1;
        }
        if (val <= Min) {
            return 0;
        }
        uint16_t range = log2floor(val);
        val >>= range - PRECISION_BITS; // leave the top most N+1 bits where N is the resolution.
        return ((range - BASESHIFT) << PRECISION_BITS) + (val & LOWER_BITS_MASK);
    }

    /*!
     * \brief clear the current values.
     */
    void clear() {
        std::fill(_buckets.begin(), _buckets.end(), 0);
    }

    /*!
     * \brief Add an item to the histogram
     * Increments the count of the bucket holding that value
     */
    void add(uint64_t n) {
        _buckets.at(find_bucket_index(n))++;
    }

    /*!
     * \brief returns the smallest value that could have been added to this histogram
     * This method looks for the first non-empty bucket and returns its lower limit.
     * Note that for non-empty histogram the lowest potentail value is Min.
     *
     * It will return 0 if the histogram is empty.
     */
    uint64_t min() const {
        for (size_t i = 0; i < NUM_BUCKETS; i ++) {
            if (_buckets[i] > 0) {
                return get_bucket_lower_limit(i);
            }
        }
        return 0;
    }

    /*!
     * \brief returns the largest value that could have been added to this histogram.
     * This method looks for the first non empty bucket and return its upper limit.
     * If the histogram overflowed, it will returns UINT64_MAX.
     *
     * It will return 0 if the histogram is empty.
     */
    uint64_t max() const {
        for (int i = NUM_BUCKETS - 1; i >= 0; i--) {
            if (_buckets[i] > 0) {
                return get_bucket_upper_limit(i);
            }
        }
        return 0;
    }

    /*!
     * \brief merge a histogram to the current one.
     */
    approximate_exponential_histogram& merge(const approximate_exponential_histogram& b) {
        for (size_t i = 0; i < NUM_BUCKETS; i++) {
            _buckets[i] += b.get(i);
        }
        return *this;
    }

    template<uint64_t A, uint64_t B, size_t C>
    friend approximate_exponential_histogram<A, B, C> merge(approximate_exponential_histogram<A, B, C> a, const approximate_exponential_histogram<A, B, C>& b);

    /*
     * \brief returns the count in the given bucket
     */
    uint64_t get(size_t bucket) const {
        return _buckets[bucket];
    }

    /*!
     * \brief get a histogram quantile
     *
     * This method will returns the estimated value at a given quantile.
     * If there are N values in the histogram.
     * It would look for the bucket that the total number of elements in the buckets
     * before it are less than N * quantile and return that bucket lower limit.
     *
     * For example, quantile(0.5) will find the bucket that that sum of all buckets values
     * below it is less than half and will return that bucket lower limit.
     * In this example, this is a median estimation.
     *
     * It will return 0 if the histogram is empty.
     *
     */
    uint64_t quantile(float quantile) const {
        if (quantile < 0 || quantile > 1.0) {
            throw std::runtime_error("Invalid quantile value " + std::to_string(quantile) + ". Value should be between 0 and 1");
        }
        auto c = count();

        if (!c) {
            return 0; // no data
        }

        auto pcount = uint64_t(std::floor(c * quantile));
        uint64_t elements = 0;
        for (size_t i = 0; i < NUM_BUCKETS - 2; i++) {
            if (_buckets[i]) {
                elements += _buckets[i];
                if (elements >= pcount) {
                    return get_bucket_lower_limit(i);
                }
            }
        }
        return Max; // overflowed value is in the requested quantile
    }

    /*!
     * \brief returns the mean histogram value (average of bucket offsets, weighted by count)
     * It will return 0 if the histogram is empty.
     */
    uint64_t mean() const {
        uint64_t elements = 0;
        double sum = 0;
        for (size_t i = 0; i < NUM_BUCKETS - 1; i++) {
            elements += _buckets[i];
            sum += _buckets[i] * get_bucket_lower_limit(i);
        }
        return (elements) ?  sum / elements : 0;
    }

    /*!
     * \brief returns the number of buckets;
     */
    size_t size() const {
        return NUM_BUCKETS;
    }

    /*!
     * \brief returns the total number of values inserted
     */
    uint64_t count() const {
        uint64_t sum = 0L;
        for (size_t i = 0; i < NUM_BUCKETS; i++) {
            sum += _buckets[i];
        }
        return sum;
    }

    /*!
     * \brief multiple all the buckets content in the histogram by a constant
     */
    approximate_exponential_histogram& operator*=(double v) {
        for (size_t i = 0; i < NUM_BUCKETS; i++) {
            _buckets[i] *= v;
        }
        return *this;
    }

    uint64_t& operator[](size_t b) noexcept {
        return _buckets[b];
    }
    /*!
     * \brief get_schema the schema of a native histogram
     *
     * Native histograms (also known as sparse histograms), are an experimental Prometheus feature.
     *
     * schema defines the bucket schema. Currently, valid numbers are -4 <= n <= 8.
     * They are all for base-2 bucket schemas, where 1 is a bucket boundary in each case, and
     * then each power of two is divided into 2^n logarithmic buckets.
     * Or in other words, each bucket boundary is the previous boundary times 2^(2^-n).
     */
    int32_t get_schema() const noexcept {
        return PRECISION_BITS;
    }

    /*!
     * \brief return the histogram min as a native histogram ID
     *
     * Native histograms (also known as sparse histograms), are an experimental Prometheus feature.
     *
     * Native histograms always starts from 1, while approximate_exponential_histogram have a min first bucket
     * This function returns approximate_exponential_histogram min value as the bucket id of native histogram.
     */
    int32_t min_as_native_histogram_id() const noexcept {
        return MIN_ID;
    }

    seastar::metrics::histogram to_metrics_histogram() const noexcept {
        seastar::metrics::histogram res;
        res.buckets.resize(size() - 1);
        uint64_t cummulative_count = 0;
        res.sample_sum = 0;
        res.native_histogram = seastar::metrics::native_histogram_info{get_schema(), min_as_native_histogram_id()};

        for (size_t i = 0; i < NUM_BUCKETS - 1; i++) {
            auto& v = res.buckets[i];
            v.upper_bound = get_bucket_lower_limit(i + 1);
            cummulative_count += get(i);
            v.count = cummulative_count;
            res.sample_sum += get(i) * v.upper_bound;
        }
        // The count serves as the infinite bucket
        res.sample_count = cummulative_count + get(NUM_BUCKETS - 1);
        res.sample_sum += get(NUM_BUCKETS - 1) * get_bucket_lower_limit(NUM_BUCKETS - 1);
        return res;
    }
};

template<uint64_t Min, uint64_t Max, size_t NumBuckets>
inline approximate_exponential_histogram<Min, Max, NumBuckets> base_estimated_histogram_merge(approximate_exponential_histogram<Min, Max, NumBuckets> a, const approximate_exponential_histogram<Min, Max, NumBuckets>& b) {
    return a.merge(b);
}

/*!
 * \brief estimated histogram for duration values
 * time_estimated_histogram is used for short task timing.
 * It covers the range of 0.5ms to 33s with a precision of 4.
 *
 * 512us, 640us, 768us, 896us, 1024us, 1280us, 1536us, 1792us...16s, 20s, 25s, 29s, 33s (33554432us)
 */
class time_estimated_histogram : public approximate_exponential_histogram<512, 33554432, 4> {
public:
    time_estimated_histogram& merge(const time_estimated_histogram& b) {
        approximate_exponential_histogram<512, 33554432, 4>::merge(b);
        return *this;
    }

    void add_micro(uint64_t n) {
        approximate_exponential_histogram<512, 33554432, 4>::add(n);
    }

    template<typename T>
    void add(const T& latency) {
        add_micro(std::chrono::duration_cast<std::chrono::microseconds>(latency).count());
    }
};

inline time_estimated_histogram time_estimated_histogram_merge(time_estimated_histogram a, const time_estimated_histogram& b) {
    return a.merge(b);
}
}
