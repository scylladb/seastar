#pragma once

#include <chrono>
#include <concepts>
#include <cstdint>
#include <type_traits>

namespace seastar {
namespace util {

///
/// Integrates frequently changing value over time
///
/// Given a volatile value (e.g. -- length of task queue), calculates a
/// time function out of it; it can later be exported as a COUNTER metrics,
/// so that in monitoring its average value can be observed by using the
/// rate() function.
///
/// The value is modified as if it was a plain unsigned integer. Every
/// modification first accounts for the time the previous value was held,
/// so the result is the exact area under the value-over-time plot, no
/// matter how rarely, or how unevenly in time, the value changes.
///
/// Every modification reads the clock. A caller that already knows the
/// current time can pass it in with set(), add() and sub() instead of using
/// the operators.
///
/// The integrator maintains three values:
/// - the value itself. It can be modified by direct assignment, or by
///   incrementing/decrementing it. It's thus a transparent wrapper of
///   unsigned integrals
///
/// - a timestamp up to which the value has been integrated
/// - the result so far. Produces the length * seconds unit. The part of it
///   that is finer than Resolution is kept apart, at the precision of the
///   clock, so that it's not lost and is not attributed to a later value
///
/// The integrated result is measured in units multiplied by seconds.
///
template <std::unsigned_integral T, typename Clock, typename Resolution = std::chrono::nanoseconds>
class integrated_length {
    // The finer of the clock's duration and Resolution, so that both convert
    // to it exactly
    using fine_duration = std::common_type_t<typename Clock::duration, Resolution>;
    static constexpr uint64_t fine_per_resolution = std::chrono::duration_cast<fine_duration>(Resolution(1)).count();

    struct integral_parts {
        uint64_t whole;         // in Resolution units
        uint64_t remainder;     // in fine_duration units, below one Resolution unit
    };

    T _value;
    uint64_t _integral;
    uint64_t _remainder;
    Clock::time_point _ts;

    // The integral accumulated since _ts, together with the remainder
    // carried over from before it
    integral_parts pending(Clock::time_point now) const noexcept {
        if (now <= _ts) {
            return {0, _remainder};
        }
        uint64_t fine = std::chrono::duration_cast<fine_duration>(now - _ts).count();
        uint64_t remainder = _remainder + uint64_t(_value) * (fine % fine_per_resolution);
        return {
            uint64_t(_value) * (fine / fine_per_resolution) + remainder / fine_per_resolution,
            remainder % fine_per_resolution,
        };
    }

public:
    /// \brief Construct an integrator initially holding zero.
    integrated_length() noexcept
            : _value(0)
            , _integral(0)
            , _remainder(0)
            , _ts(Clock::now())
    {}

    /// \brief Construct an integrator with the given initial value.
    integrated_length(T iv) noexcept
            : _value(iv)
            , _integral(0)
            , _remainder(0)
            , _ts(Clock::now())
    {}

    /// \brief Compare the current values of two integrators.
    bool operator==(const integrated_length& o) const noexcept {
        return _value == o._value;
    }

    /// \brief Compare the current values for inequality.
    bool operator!=(const integrated_length& o) const noexcept {
        return _value != o._value;
    }

    // Below methods transparently "wrap" the T type for arithmetics.
    // Each of them integrates the current value up to now before
    // changing it.

    /// \brief Set the value, integrating the preceding interval.
    void operator=(T v) noexcept {
        set(v, Clock::now());
    }

    /// \brief Increment the current value.
    integrated_length& operator++() noexcept {
        add(1, Clock::now());
        return *this;
    }

    /// \brief Post-increment and return the previous wrapper.
    integrated_length operator++(int) noexcept {
        integrated_length previous(*this);
        add(1, Clock::now());
        return previous;
    }

    /// \brief Decrement the current value.
    integrated_length& operator--() noexcept {
        sub(1, Clock::now());
        return *this;
    }

    /// \brief Post-decrement and return the previous wrapper.
    integrated_length operator--(int) noexcept {
        integrated_length previous(*this);
        sub(1, Clock::now());
        return previous;
    }

    /// \brief Compare the current value with an unsigned integer.
    bool operator==(const T& v) const noexcept {
        return _value == v;
    }

    /// \brief Compare the current value for inequality.
    bool operator!=(const T& v) const noexcept {
        return _value != v;
    }

    // Same as the operators above, but with the current time provided by
    // the caller (e.g. for tests or optimizations).

    /// \brief Set the value at a caller-provided time point.
    void set(T v, Clock::time_point now) noexcept {
        checkpoint(now);
        _value = v;
    }

    /// \brief Add to the value at a caller-provided time point.
    void add(T v, Clock::time_point now) noexcept {
        checkpoint(now);
        _value += v;
    }

    /// \brief Subtract from the value at a caller-provided time point.
    void sub(T v, Clock::time_point now) noexcept {
        checkpoint(now);
        _value -= v;
    }

    /// \brief Integrate the current value up to \p now without changing it.
    ///
    /// The modifications above do it on their own, so calling it is never
    /// required for correctness. A time point earlier than the last one is
    /// ignored.
    void checkpoint(Clock::time_point now = Clock::now()) noexcept {
        if (now <= _ts) {
            return;
        }
        auto parts = pending(now);
        _integral += parts.whole;
        _remainder = parts.remainder;
        _ts = now;
    }

    /// \brief Return the current value.
    T value() const noexcept {
        return _value;
    }

    /// \brief Return the integral up to \p now, in value-seconds.
    ///
    /// The current value counts for the time it has been held since the
    /// last change. The result is rounded down to whole value-seconds.
    uint64_t integral(Clock::time_point now) const noexcept {
        // Implicitly casts time component to seconds
        return (_integral + pending(now).whole) / std::chrono::duration_cast<Resolution>(std::chrono::seconds(1)).count();
    }

    /// \brief Return the integral up to the current time, in value-seconds.
    ///
    /// Same as integral(Clock::now()). A value that stays the same keeps the
    /// result growing even without modifications or checkpoints.
    uint64_t integral() const noexcept {
        return integral(Clock::now());
    }
};

} // util namespace
} // seastar namespace
