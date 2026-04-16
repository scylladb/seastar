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

/*
 * A token bucket where each competing class accumulates its proportional share
 * of tokens independently, based on its shares relative to the total.
 *
 * The global state holds a monotonically increasing `tokens_generated` counter
 * that any shard can advance by calling replenish(). Each class has shard-local
 * state and computes its token allocation lazily as:
 *
 *   gain = (tokens_generated - last_seen) * class_shares / total_shares
 *
 * This means a "stuck" shard that doesn't poll for a while simply catches up
 * to its burst cap when it wakes up, without being starved by other shards.
 * Conversely, a fast-polling shard cannot grab tokens that belong to other
 * classes — each class's share is computed independently from the shared clock.
 *
 * Architecture:
 *
 *                     +-------------------------------+
 *                     |   shard_aware_token_bucket    |
 *                     |                               |
 *                     |  _rate                        | <-- tokens per second
 *                     |  _tokens_generated (atomic)   | <-- replenish() advances this
 *                     |  _total_shares     (atomic)   | <-- sum of all active consumers
 *                     |  _tau_tokens                  | <-- forgiveness window
 *                     +---------------+---------------+
 *                                     |
 *              +----------------------+----------------------------+
 *              |                                                   |
 *    +---------v-----------------+                       +---------v--------+
 *    | local_publisher (shard 0) |                       | local_publisher  |
 *    |                           |                       |    (shard 1)     |
 *    |  _pending                 | <-- accumulated       +--+----------+---+
 *    |                           |     delta-shares         |          |
 *    |  flush()                  | <-- commits           consumer  consumer
 *    |                           |     _pending to
 *    +--+-------------------+----+     _total_shares
 *       |                   |          in one atomic RMW
 *       |                   |
 *  +----v-----------+  +----v-----------+
 *  | consumer       |  | consumer       |
 *  |                |  +----------------+
 *  |                |
 *  |  _shares       | <-- weight in the proportional split
 *  |  _last_seen    | <-- snapshot of _tokens_generated at previous accrued()
 *  |  accrued()     | <-- returns (generated - _last_seen) * _shares / total_shares
 *  |  activate()    | <-- adds _shares to _total_shares via publisher
 *  |  deactivate()  | <-- removes _shares from _total_shares via publisher
 *  +----------------+
 *
 * Per-poll data flow (one shard):
 *
 *  1. replenish(now) -- any shard may advance _tokens_generated based on
 *     elapsed time; concurrent calls are serialized via CAS.
 *
 *  2. local_publisher::flush() -- commits locally batched share deltas to
 *     the global _total_shares in a single atomic fetch-and-add.
 *
 *  3. consumer::accrued() -- each consumer computes its token gain since the
 *     last call as (generated - _last_seen) * _shares / total_shares, then
 *     updates _last_seen. The result is a plain token value returned to the
 *     caller (io_queue's dispatch_root) for redistribution.
 *
 *  4. activate/deactivate -- when a class becomes active or idle, its shares
 *     are added to or removed from _pending in the local_publisher, to be
 *     committed on the next flush().
 */
template <typename T, typename Period, typename Clock = std::chrono::steady_clock>
requires std::is_nothrow_copy_constructible_v<T> && supports_wrapping_arithmetics<T>
class shard_aware_token_bucket {
    using rate_resolution = std::chrono::duration<double, Period>;

    T _rate;
    const T _burst_limit;
    const T _threshold;
    const T _tau_tokens;
    // Monotonically increasing counter of tokens produced globally.
    // replenish() converts elapsed wall-clock time into tokens and
    // advances this via atomic fetch-add. Consumers read it in
    // accrued() and compute their per-class share of the delta
    // relative to their own _last_seen snapshot (see consumer below).
    std::atomic<T> _tokens_generated{0};
    std::atomic<typename Clock::time_point> _last_replenished;
    std::atomic<T> _total_shares{0};

    static constexpr rate_resolution max_delta = std::chrono::duration_cast<rate_resolution>(std::chrono::hours(1));

    T accumulated_in(typename Clock::duration delta) const noexcept {
        auto d = std::min(std::chrono::duration_cast<rate_resolution>(delta), max_delta);
        return T(std::round(_rate * d.count()));
    }

public:
    static constexpr T max_rate = std::numeric_limits<T>::max() / 2 / max_delta.count();

    template <typename Rep, typename Per>
    static auto rate_cast(const std::chrono::duration<Rep, Per> d) noexcept {
        return std::chrono::duration_cast<rate_resolution>(d);
    }

    rate_resolution duration_for(T tokens) const noexcept {
        return rate_resolution(double(tokens) / _rate);
    }

    T rate() const noexcept { return _rate; }
    T limit() const noexcept { return _burst_limit; }
    T threshold() const noexcept { return _threshold; }
    T total_shares() const noexcept { return _total_shares.load(std::memory_order_relaxed); }
    T tokens_generated() const noexcept { return _tokens_generated.load(std::memory_order_relaxed); }

    T tau_tokens() const noexcept { return _tau_tokens; }

    shard_aware_token_bucket(T rate, T burst_limit, T threshold,
                              bool add_replenish_offset = true,
                              typename Clock::duration tau = {}) noexcept
        : _rate(std::min(rate, max_rate))
        , _burst_limit(burst_limit)
        , _threshold(std::clamp(threshold, T(1), burst_limit))
        , _tau_tokens(accumulated_in(tau))
        , _last_replenished(Clock::now() - std::chrono::hours(add_replenish_offset ? 24 : 0))
    {}

    /*
     * Advance the global token counter based on elapsed time.
     * Only one shard succeeds per threshold window via CAS on _last_replenished.
     */
    void replenish(typename Clock::time_point now) noexcept {
        auto ts = _last_replenished.load(std::memory_order_relaxed);
        if (now <= ts) {
            return;
        }
        auto extra = accumulated_in(now - ts);
        if (extra >= _threshold) {
            if (!_last_replenished.compare_exchange_strong(ts, now)) {
                return;
            }
            _tokens_generated.fetch_add(extra, std::memory_order_relaxed);
        }
    }

    /*
     * A shard-local token pouch. Carries a number of currently-available
     * tokens and enforces a burst cap on top-ups. Has no ties to the bucket
     * or to any particular token source: it is fed via refill(amount) from
     * whatever source the owner chooses (typically either consumer::accrued()
     * for a direct draw from the global bucket, or a raw amount passed down
     * from a parent entity redistributing its own drained tokens).
     */
    class tokens {
        T _available{0};
        T _burst_limit;

    public:
        explicit tokens(T burst_limit) noexcept : _burst_limit(burst_limit) {}

        /*
         * Add amount tokens to the pouch, saturating at burst_limit. The cap
         * prevents unbounded accumulation while the owner is idle or stalled.
         */
        void refill(T amount) noexcept {
            _available = std::min(_available + amount, _burst_limit);
        }

        /*
         * Consume cost tokens. Returns true and deducts if enough are available.
         */
        bool try_consume(T cost) noexcept {
            if (_available >= cost) {
                _available -= cost;
                return true;
            }
            return false;
        }

        /* Empty the pouch and return what was in it (for redistribution). */
        T drain() noexcept {
            return std::exchange(_available, T(0));
        }

        T available() const noexcept { return _available; }
    };

    /*
     * Per-class, shard-local share/generation bookkeeping. Not thread-safe on
     * its own; intended to be owned and accessed by a single shard.
     *
     * A class starts inactive and does not contribute to total_shares until
     * activate() is called. Call activate() when the class first has pending
     * requests, and deactivate() when it becomes idle again.
     *
     * Tokens generated for this consumer are not kept here; use accrued() to
     * compute them and feed the result into an externally-owned tokens pouch.
     */
    class consumer {
        shard_aware_token_bucket& _bucket;
        T _shares;
        // Per-consumer watermark into _tokens_generated (see above).
        // accrued() computes gain as (generated - _last_seen) * _shares
        // / total_shares, then advances _last_seen to generated. On
        // re-activation after idle, _last_seen is clamped so the class
        // receives at most tau_tokens worth of catch-up credit.
        T _last_seen;
        bool _active{false};

    public:
        consumer(shard_aware_token_bucket& bucket, T shares) noexcept
            : _bucket(bucket)
            , _shares(shares)
            , _last_seen(bucket._tokens_generated.load(std::memory_order_relaxed))
        {}

        consumer(const consumer&) = delete;
        consumer(consumer&&) = delete;

        ~consumer() {
            if (_active) {
                deactivate();
            }
        }

        /*
         * Mark this class as actively competing for tokens. Contributes _shares
         * to total_shares until deactivate() is called.
         *
         * On re-activation after idle, the class receives credit for at most
         * tau_tokens worth of global generation (the forgiveness window). This
         * allows briefly-idle classes to burst on return without granting
         * unbounded catch-up to long-idle ones.
         */
        void activate() noexcept {
            SEASTAR_ASSERT(!_active);
            _active = true;
            auto gen = _bucket._tokens_generated.load(std::memory_order_relaxed);
            auto tau = _bucket._tau_tokens;
            if (tau > 0 && gen >= tau) {
                _last_seen = std::max(_last_seen, gen - tau);
            } else if (tau == 0) {
                _last_seen = gen;
            }
            _bucket._total_shares.fetch_add(_shares, std::memory_order_relaxed);
        }

        /* Remove this class from active competition. */
        void deactivate() noexcept {
            SEASTAR_ASSERT(_active);
            _active = false;
            _bucket._total_shares.fetch_sub(_shares, std::memory_order_relaxed);
        }

        bool is_active() const noexcept { return _active; }

        /*
         * Return the number of tokens proportionally produced for this consumer
         * since the previous call (or since construction / reactivation), and
         * advance the internal watermark. The caller is expected to feed the
         * result into a tokens pouch via tokens::refill().
         *
         * Fairness is preserved by the proportional gain formula; no capping
         * is applied here -- the pouch's burst_limit caps accumulation.
         */
        T accrued() noexcept {
            auto gen = _bucket._tokens_generated.load(std::memory_order_relaxed);
            auto delta = gen - _last_seen;
            if (delta == 0) {
                return T(0);
            }
            _last_seen = gen;
            auto total = _bucket._total_shares.load(std::memory_order_relaxed);
            if (total == 0) {
                return T(0);
            }
            return T(double(delta) * _shares / total);
        }

        /* Update shares; adjusts total_shares immediately if currently active. */
        void update_shares(T new_shares) noexcept {
            if (_active) {
                if (new_shares >= _shares) {
                    _bucket._total_shares.fetch_add(new_shares - _shares, std::memory_order_relaxed);
                } else {
                    _bucket._total_shares.fetch_sub(_shares - new_shares, std::memory_order_relaxed);
                }
            }
            _shares = new_shares;
        }

        T shares() const noexcept { return _shares; }
    };
};

} // internal namespace
} // seastar namespace
