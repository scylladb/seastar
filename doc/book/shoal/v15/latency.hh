// A histogram of latencies, for the metrics of version 15.
#pragma once

#include <seastar/core/metrics_types.hh>
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>

// snippet: latency-histogram
// Counts latencies in buckets of powers of two of microseconds: up to 1 µs,
// up to 2 µs, up to 4 µs, and so on, up to about 4 s, and the last bucket
// for anything longer, which Prometheus counts only in the total. Adding a
// latency is a few instructions, and only a collection of the metrics,
// every few seconds, builds the histogram that Prometheus wants, in which
// each bucket counts the latencies up to its bound, the earlier buckets'
// too, and the bounds are in seconds.
class latency_histogram {
    static constexpr size_t buckets = 24;
    std::array<uint64_t, buckets> _counts{};
    uint64_t _count = 0;
    double _sum = 0;            // in seconds
public:
    void add(std::chrono::steady_clock::duration d) {
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(d)
                      .count();
        // The bucket whose bound is the smallest power of two >= us.
        size_t i = us <= 1 ? 0 : std::bit_width(uint64_t(us - 1));
        _counts[std::min(i, buckets - 1)]++;
        _count++;
        _sum += std::chrono::duration<double>(d).count();
    }
    seastar::metrics::histogram to_metrics() const {
        seastar::metrics::histogram h;
        h.sample_count = _count;
        h.sample_sum = _sum;
        uint64_t below = 0;
        for (size_t i = 0; i + 1 < buckets; i++) {
            below += _counts[i];
            h.buckets.push_back({below, double(uint64_t(1) << i) / 1e6});
        }
        return h;
    }
};
// end snippet: latency-histogram
