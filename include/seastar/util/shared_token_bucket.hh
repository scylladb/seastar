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
 * Copyright (C) 2022 ScyllaDB
 */

#pragma once

#include <atomic>
#include <chrono>
#include <cmath>
#include <concepts>
#include <cstdint>

namespace seastar {
namespace internal {

inline uint64_t wrapping_difference(const uint64_t& a, const uint64_t& b) noexcept {
    return std::max<int64_t>(a - b, 0);
}

inline uint64_t fetch_add(std::atomic<uint64_t>& a, uint64_t b) noexcept {
    return a.fetch_add(b);
}

template <typename T>
concept supports_wrapping_arithmetics = requires (T a, std::atomic<T> atomic_a, T b) {
    { fetch_add(atomic_a, b) } noexcept -> std::same_as<T>;
    { wrapping_difference(a, b) } noexcept -> std::same_as<T>;
    { a + b } noexcept -> std::same_as<T>;
};

enum class capped_release { yes, no };

template <typename T, capped_release Capped>
struct rovers;

template <typename T>
struct rovers<T, capped_release::yes> {
    using atomic_rover = std::atomic<T>;

    atomic_rover tail;
    atomic_rover head;
    atomic_rover ceil;

    rovers(T limit) noexcept : tail(0), head(0), ceil(limit) {}

    T max_extra(T) const noexcept {
        return wrapping_difference(ceil.load(std::memory_order_relaxed), head.load(std::memory_order_relaxed));
    }

    void release(T tokens) {
        fetch_add(ceil, tokens);
    }
};

template <typename T>
struct rovers<T, capped_release::no> {
    using atomic_rover = std::atomic<T>;

    atomic_rover tail;
    atomic_rover head;

    rovers(T) noexcept : tail(0), head(0) {}

    T max_extra(T limit) const noexcept {
        return wrapping_difference(tail.load(std::memory_order_relaxed) + limit, head.load(std::memory_order_relaxed));
    }

    void release(T) = delete;
};

template <typename T, typename Period, capped_release Capped, typename Clock = std::chrono::steady_clock>
requires std::is_nothrow_copy_constructible_v<T> && supports_wrapping_arithmetics<T>
class shared_token_bucket {
    using rate_resolution = std::chrono::duration<double, Period>;

    T _replenish_rate;
    const T _replenish_limit;
    const T _replenish_threshold;
    std::atomic<typename Clock::time_point> _replenished;

    /*
     * The token bucket is implemented as a pair of wrapping monotonic
     * counters (called rovers) one chasing the other. Getting a token
     * from the bucket is increasing the tail, replenishing a token back
     * is increasing the head. If increased tail overruns the head then
     * the bucket is empty and we have to wait. The shard that grabs tail
     * earlier will be "woken up" earlier, so they form a queue.
     *
     * The top rover is needed to implement two buckets actually. The
     * tokens are not just replenished by timer. They are replenished by
     * timer from the second bucket. And the second bucket only get a
     * token in it after the request that grabbed it from the first bucket
     * completes and returns it back.
     */

    using rovers_t = rovers<T, Capped>;
    static_assert(rovers_t::atomic_rover::is_always_lock_free);
    rovers_t _rovers;

    T tail() const noexcept { return _rovers.tail.load(std::memory_order_relaxed); }
    T head() const noexcept { return _rovers.head.load(std::memory_order_relaxed); }

    /*
     * Need to make sure that the multiplication in accumulated_in() doesn't
     * overflow. Not to introduce an extra branch there, define that the
     * replenish period is not larger than this delta and limit the rate with
     * the value that can overflow it.
     *
     * The additional /=2 in max_rate math is to make extra sure that the
     * overflow doesn't break wrapping_difference sign tricks.
     */
    static constexpr rate_resolution max_delta = std::chrono::duration_cast<rate_resolution>(std::chrono::hours(1));
public:
    static constexpr T max_rate = std::numeric_limits<T>::max() / 2 / max_delta.count();
    static constexpr capped_release is_capped = Capped;

private:
    static constexpr T accumulated(T rate, rate_resolution delta) noexcept {
        return std::round(rate * delta.count());
    }
#ifndef __clang__
    // std::round() is constexpr only since C++23 (but g++ doesn't care)
    static_assert(accumulated(max_rate, max_delta) <= std::numeric_limits<T>::max());
#endif

public:
    shared_token_bucket(T rate, T limit, T threshold, bool add_replenish_iffset = true) noexcept
            : _replenish_rate(std::min(rate, max_rate))
            , _replenish_limit(limit)
            , _replenish_threshold(std::clamp(threshold, (T)1, limit))
            // pretend it was replenished yesterday to spot overflows early
            , _replenished(Clock::now() - std::chrono::hours(add_replenish_iffset ? 24 : 0))
            , _rovers(_replenish_limit)
    {}

    T grab(T tokens) noexcept {
        return fetch_add(_rovers.tail, tokens) + tokens;
    }

    void release(T tokens) noexcept {
        _rovers.release(tokens);
    }

    void refund(T tokens) noexcept {
        fetch_add(_rovers.head, tokens);
    }

    void replenish(typename Clock::time_point now) noexcept {
        auto ts = _replenished.load(std::memory_order_relaxed);

        if (now <= ts) {
            return;
        }

        auto delta = now - ts;
        auto extra = accumulated_in(delta);

        if (extra >= _replenish_threshold) {
            if (!_replenished.compare_exchange_weak(ts, ts + delta)) {
                return; // next time or another shard
            }

            fetch_add(_rovers.head, std::min(extra, _rovers.max_extra(_replenish_limit)));
        }
    }

    T deficiency(T from) const noexcept {
        return wrapping_difference(from, head());
    }

    template <typename Rep, typename Per>
    static auto rate_cast(const std::chrono::duration<Rep, Per> delta) noexcept {
        return std::chrono::duration_cast<rate_resolution>(delta);
    }

    // the number of tokens accumulated for the given time frame
    template <typename Rep, typename Per>
    T accumulated_in(const std::chrono::duration<Rep, Per> delta) const noexcept {
       auto delta_at_rate = std::min(rate_cast(delta), max_delta);
       return accumulated(_replenish_rate, delta_at_rate);
    }

    // Estimated time to process the given amount of tokens
    // (peer of accumulated_in helper)
    rate_resolution duration_for(T tokens) const noexcept {
        return rate_resolution(double(tokens) / _replenish_rate);
    }

    T rate() const noexcept { return _replenish_rate; }
    T limit() const noexcept { return _replenish_limit; }
    T threshold() const noexcept { return _replenish_threshold; }
    typename Clock::time_point replenished_ts() const noexcept { return _replenished; }

    void update_rate(T rate) noexcept {
        _replenish_rate = std::min(rate, max_rate);
    }
};

} // internal namespace
} // seastar namespace
