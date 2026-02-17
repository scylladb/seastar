#pragma once

namespace seastar {
namespace util {

//
// Integrates frequently changing value over time
//
// Given a volatile value (e.g. -- length of task queue), calculates a
// time function out of it; it can later be exported as a COUNTER metrics,
// so that in monitoring its average value can be observed by using the
// rate() function.
//
// The value is modified as if it was a plain unsigned integer, and at
// some points it must be explicitly checkpointed by calling the respective
// method of this class. Only the checkpoint-ed values contribute to the
// final result, so observed in metrics will be those values either.
// However, checkpointing the value on every update is possible.
//
// The integrator maintains three values:
// - the value itself. It can be modified by direct assignment, or by
//   incrementing/decrementing it. It's thus a transparent wrapper of
//   unsigned integrals
//
// - a timestamp of the last checkpoint
// - the final result. Produces the length * seconds unit
//
// The calculated value is the sum of value * (now - timestamp) for every
// checkpoint seen so far. The integrated result is measured in units
// multiplied by seconds.
//
template <std::unsigned_integral T, typename Clock, typename Resolution = std::chrono::nanoseconds>
class integrated_length {
    T _value;
    uint64_t _integral;
    Clock::time_point _ts;

public:
    integrated_length() noexcept
            : _value(0)
            , _integral(0)
            , _ts(Clock::now())
    {}

    integrated_length(T iv) noexcept
            : _value(iv)
            , _integral(0)
            , _ts(Clock::now())
    {}

    bool operator==(const integrated_length& o) const noexcept {
        return _value == o._value;
    }

    bool operator!=(const integrated_length& o) const noexcept {
        return _value != o._value;
    }

    // Below methods transparently "wrap" the T type for arithmetics

    void operator=(T v) noexcept {
        _value = v;
    }

    integrated_length& operator++() noexcept {
        _value++;
        return *this;
    }

    integrated_length operator++(int) noexcept {
        integrated_length previous(*this);
        _value++;
        return previous;
    }

    integrated_length& operator--() noexcept {
        _value--;
        return *this;
    }

    integrated_length operator--(int) noexcept {
        integrated_length previous(*this);
        _value--;
        return previous;
    }

    bool operator==(const T& v) const noexcept {
        return _value == v;
    }

    bool operator!=(const T& v) const noexcept {
        return _value != v;
    }

    // Updates the integral value by adding yet another
    // value * duration component. The "now" time-point can be
    // provided (e.g. for tests or optimizations)
    void checkpoint(Clock::time_point now = Clock::now()) noexcept {
        Resolution dur = std::chrono::duration_cast<Resolution>(now - _ts);
        _ts = now;
        _integral += _value * dur.count();
    }

    // Returns the last seen (or current) value
    T value() const noexcept {
        return _value;
    }

    // Returns the integrated result
    uint64_t integral() const noexcept {
        // Implicitly casts time component to seconds
        return _integral / std::chrono::duration_cast<Resolution>(std::chrono::seconds(1)).count();
    }
};

} // util namespace
} // seastar namespace
