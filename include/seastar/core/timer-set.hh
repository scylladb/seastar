/*
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 */

/*
 * Imported from OSv:
 *
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#pragma once

#include <seastar/core/bitset-iter.hh>
#include <seastar/core/scheduling.hh>
#include <seastar/util/assert.hh>
#ifndef SEASTAR_MODULE
#include <boost/intrusive/list.hpp>
#include <exception>
#include <array>
#include <bitset>
#include <limits>
#endif

namespace seastar {

namespace internal {
void log_timer_callback_exception(std::exception_ptr) noexcept;
}

/**
 * A data structure designed for holding and expiring timers. It's
 * optimized for timer non-delivery by deferring sorting cost until
 * expiry time. The optimization is based on the observation that in
 * many workloads timers are cancelled or rescheduled before they
 * expire. That's especially the case for TCP timers.
 *
 * The template type "Timer" should have a method named
 * get_timeout() which returns Timer::time_point which denotes
 * timer's expiration.
 */
template<typename Timer, boost::intrusive::list_member_hook<> Timer::*link>
class timer_set {
public:
    using time_point = typename Timer::time_point;
    using timer_list_t = boost::intrusive::list<Timer, boost::intrusive::member_hook<Timer, boost::intrusive::list_member_hook<>, link>>;
private:
    using duration = typename Timer::duration;
    using timestamp_t = typename Timer::duration::rep;

    static constexpr timestamp_t max_timestamp = std::numeric_limits<timestamp_t>::max();
    static constexpr int timestamp_bits = std::numeric_limits<timestamp_t>::digits;

    // The last bucket is reserved for active timers with timeout <= _last.
    static constexpr int n_buckets = timestamp_bits + 1;

    std::array<timer_list_t, n_buckets> _buckets;
    timestamp_t _last;
    timestamp_t _next;

    std::bitset<n_buckets> _non_empty_buckets;
private:
    static timestamp_t get_timestamp(time_point _time_point) noexcept
    {
        return _time_point.time_since_epoch().count();
    }

    static timestamp_t get_timestamp(Timer& timer) noexcept
    {
        return get_timestamp(timer.get_timeout());
    }

    int get_index(timestamp_t timestamp) const noexcept
    {
        if (timestamp <= _last) {
            return n_buckets - 1;
        }

        auto index = bitsets::count_leading_zeros(timestamp ^ _last);
        SEASTAR_ASSERT(index < n_buckets - 1);
        return index;
    }

    int get_index(Timer& timer) const noexcept
    {
        return get_index(get_timestamp(timer));
    }

    int get_last_non_empty_bucket() const noexcept
    {
        return bitsets::get_last_set(_non_empty_buckets);
    }

public:
    timer_set() noexcept
        : _last(0)
        , _next(max_timestamp)
        , _non_empty_buckets(0)
    {
    }

    ~timer_set() {
        for (auto&& list : _buckets) {
            while (!list.empty()) {
                auto& timer = *list.begin();
                timer.cancel();
            }
        }
    }

    /**
     * Adds timer to the active set.
     *
     * The value returned by timer.get_timeout() is used as timer's expiry. The result
     * of timer.get_timeout() must not change while the timer is in the active set.
     *
     * Preconditions:
     *  - this timer must not be currently in the active set or in the expired set.
     *
     * Postconditions:
     *  - this timer will be added to the active set until it is expired
     *    by a call to expire() or removed by a call to remove().
     *
     * Returns true if and only if this timer's timeout is less than get_next_timeout().
     * When this function returns true the caller should reschedule expire() to be
     * called at timer.get_timeout() to ensure timers are expired in a timely manner.
     */
    bool insert(Timer& timer) noexcept
    {
        auto timestamp = get_timestamp(timer);
        auto index = get_index(timestamp);

        _buckets[index].push_back(timer);
        _non_empty_buckets[index] = true;

        if (timestamp < _next) {
            _next = timestamp;
            return true;
        }
        return false;
    }

    /**
     * Removes timer from the active set.
     *
     * Preconditions:
     *  - timer must be currently in the active set. Note: it must not be in
     *    the expired set.
     *
     * Postconditions:
     *  - timer is no longer in the active set.
     *  - this object will no longer hold any references to this timer.
     */
    void remove(Timer& timer) noexcept
    {
        auto index = get_index(timer);
        auto& list = _buckets[index];
        list.erase(list.iterator_to(timer));
        if (list.empty()) {
            _non_empty_buckets[index] = false;
        }
    }

    /**
     * Removes timer from the active set or the expired list, if the timer is expired
     */
    void remove(Timer& timer, timer_list_t& expired) noexcept
    {
        if (timer._expired) {
            expired.erase(expired.iterator_to(timer));
            timer._expired = false;
        } else {
            remove(timer);
        }
    }

    /**
     * Expires active timers.
     *
     * The time points passed to this function must be monotonically increasing.
     * Use get_next_timeout() to query for the next time point.
     *
     * Preconditions:
     *  - the time_point passed to this function must not be lesser than
     *    the previous one passed to this function.
     *
     * Postconditons:
     *  - all timers from the active set with Timer::get_timeout() <= now are moved
     *    to the expired set.
     */
    timer_list_t expire(time_point now) noexcept
    {
        timer_list_t exp;
        auto timestamp = get_timestamp(now);

        if (timestamp < _last) {
            abort();
        }

        auto index = get_index(timestamp);

        for (int i : bitsets::for_each_set(_non_empty_buckets, index + 1)) {
            exp.splice(exp.end(), _buckets[i]);
            _non_empty_buckets[i] = false;
        }

        _last = timestamp;
        _next = max_timestamp;

        auto& list = _buckets[index];
        while (!list.empty()) {
            auto& timer = *list.begin();
            list.pop_front();
            if (timer.get_timeout() <= now) {
                exp.push_back(timer);
            } else {
                insert(timer);
            }
        }

        _non_empty_buckets[index] = !list.empty();

        if (_next == max_timestamp && _non_empty_buckets.any()) {
            for (auto& timer : _buckets[get_last_non_empty_bucket()]) {
                _next = std::min(_next, get_timestamp(timer));
            }
        }
        return exp;
    }

    template <typename EnableFunc>
    void complete(timer_list_t& expired_timers, EnableFunc&& enable_fn) noexcept(noexcept(enable_fn())) {
        expired_timers = expire(this->now());
        for (auto& t : expired_timers) {
            t._expired = true;
        }
        const auto prev_sg = current_scheduling_group();
        while (!expired_timers.empty()) {
            auto t = &*expired_timers.begin();
            expired_timers.pop_front();
            t->_queued = false;
            if (t->_armed) {
                t->_armed = false;
                if (t->_period) {
                    t->readd_periodic();
                }
                try {
                    *internal::current_scheduling_group_ptr() = t->_sg;
                    t->_callback();
                } catch (...) {
                    internal::log_timer_callback_exception(std::current_exception());
                }
            }
        }
        // complete_timers() can be called from the context of run_tasks()
        // as well so we need to restore the previous scheduling group (set by run_tasks()).
        *internal::current_scheduling_group_ptr() = prev_sg;
        enable_fn();
    }

    /**
     * Returns a time point at which expire() should be called
     * in order to ensure timers are expired in a timely manner.
     *
     * Returned values are monotonically increasing.
     */
    time_point get_next_timeout() const noexcept
    {
        return time_point(duration(std::max(_last, _next)));
    }

    /**
     * Clears both active and expired timer sets.
     */
    void clear() noexcept
    {
        for (int i : bitsets::for_each_set(_non_empty_buckets)) {
            _buckets[i].clear();
        }
    }

    size_t size() const noexcept
    {
        size_t res = 0;
        for (int i : bitsets::for_each_set(_non_empty_buckets)) {
            res += _buckets[i].size();
        }
        return res;
    }

    /**
     * Returns true if and only if there are no timers in the active set.
     */
    bool empty() const noexcept
    {
        return _non_empty_buckets.none();
    }

    time_point now() noexcept {
        return Timer::clock::now();
    }
};
}
