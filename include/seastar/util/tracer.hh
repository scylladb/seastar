#pragma once

#include <deque>
#include <vector>
#include <cstdint>
#include <cstring>
#include <seastar/core/byteorder.hh>

namespace seastar {

struct tracer {
    static constexpr size_t buffer_size = (128 * 1024);
    std::deque<std::vector<std::byte>> _old;
    std::vector<std::byte> _current;
    size_t _cur_pos = 0;

    tracer() {
        for (int i = 0; i < 480; ++i) {
            _old.push_back(std::vector<std::byte>());
        }
        _current.resize(buffer_size);
    }

    void rotate() {
        _current.resize(_cur_pos);
        _old.push_back(std::move(_current));
        _current = std::move(_old.front());
        _old.pop_front();
        _current.resize(buffer_size);
        _cur_pos = 0;
    }

    std::byte* write(size_t n) {
        if (_current.size() - _cur_pos < n) [[unlikely]] {
            rotate();
        }
        auto result = &_current[_cur_pos];
        _cur_pos += n;
        return result;
    }

    std::deque<std::vector<std::byte>> snapshot() {
        auto result = _old;
        auto cur = _current;
        cur.resize(_cur_pos);
        result.push_back(cur);
        return result;
    }

    uint64_t rdtsc() {
        uint64_t rax, rdx;
        asm volatile ( "rdtsc" : "=a" (rax), "=d" (rdx) );
        return (uint64_t)(( rdx << 32 ) + rax);
    }
};
extern thread_local tracer g_tracer;


enum class trace_events {
    POLL,
    SLEEP,
    WAKEUP,
    RUN_TASK_QUEUE,
    RUN_TASK_QUEUE_END,
    RUN_TASK,
    RUN_EXECUTION_STAGE_TASK,
    GRAB_CAPACITY,
    GRAB_CAPACITY_PENDING,
    DISPATCH_REQUESTS,
    DISPATCH_QUEUE,
    REPLENISH,
    IO_QUEUE,
    IO_DISPATCH,
    IO_COMPLETE,
    IO_CANCEL,
    MONITORING_SCRAPE,
    COUNT,
};

[[gnu::always_inline]]
inline void tracepoint_nullary(trace_events header) {
    auto p = reinterpret_cast<char*>(g_tracer.write(12));
    p = seastar::write_le<uint32_t>(p, static_cast<uint32_t>(header));
    p = seastar::write_le<uint64_t>(p, g_tracer.rdtsc());
}

template <typename T>
[[gnu::always_inline]]
inline void tracepoint_unary(uint32_t header, T arg) {
    auto p = reinterpret_cast<char*>(g_tracer.write(12 + sizeof(T)));
    p = seastar::write_le<uint32_t>(p, static_cast<uint32_t>(header));
    p = seastar::write_le<uint64_t>(p, g_tracer.rdtsc());
    p = seastar::write_le<T>(p, arg);
}

template <typename T>
[[gnu::always_inline]]
inline void tracepoint_unary(trace_events header, T arg) {
    tracepoint_unary(static_cast<uint32_t>(header), arg);
}

inline void tracepoint_poll() {
    tracepoint_nullary(trace_events::POLL);
}

inline void tracepoint_sleep() {
    tracepoint_nullary(trace_events::SLEEP);
}

inline void tracepoint_wakeup() {
    tracepoint_nullary(trace_events::WAKEUP);
}

inline void tracepoint_run_task_queue(uint8_t sg) {
    tracepoint_unary<uint8_t>(trace_events::RUN_TASK_QUEUE, sg);
}

inline void tracepoint_run_task_queue_end() {
    tracepoint_nullary(trace_events::RUN_TASK_QUEUE_END);
}

inline void tracepoint_run_task(int64_t task_id) {
    tracepoint_unary<uint64_t>(trace_events::RUN_TASK, task_id);
}

inline void tracepoint_run_execution_stage_task(int64_t task_id) {
    tracepoint_unary<uint64_t>(trace_events::RUN_EXECUTION_STAGE_TASK, task_id);
}

inline void tracepoint_io_queue(uint8_t direction, uint64_t tokens, uint64_t io_id) {
    auto p = reinterpret_cast<char*>(g_tracer.write(12 + 17));
    p = seastar::write_le<uint32_t>(p, static_cast<uint32_t>(trace_events::IO_QUEUE));
    p = seastar::write_le<uint64_t>(p, g_tracer.rdtsc());
    p = seastar::write_le<uint8_t>(p, direction);
    p = seastar::write_le<uint64_t>(p, tokens);
    p = seastar::write_le<uint64_t>(p, io_id);
}

inline void tracepoint_io_dispatch(uint64_t io_id) {
    tracepoint_unary<uint64_t>(trace_events::IO_DISPATCH, io_id);
}

inline void tracepoint_io_complete(uint64_t io_id) {
    tracepoint_unary<uint64_t>(trace_events::IO_COMPLETE, io_id);
}

inline void tracepoint_io_cancel(uint64_t io_id) {
    tracepoint_unary<uint64_t>(trace_events::IO_CANCEL, io_id);
}

inline void tracepoint_grab_capacity(uint64_t cap, uint64_t want_head, uint64_t head) {
    auto p = reinterpret_cast<char*>(g_tracer.write(12 + 24));
    p = seastar::write_le<uint32_t>(p, static_cast<uint32_t>(trace_events::GRAB_CAPACITY));
    p = seastar::write_le<uint64_t>(p, g_tracer.rdtsc());
    p = seastar::write_le<uint64_t>(p, cap);
    p = seastar::write_le<uint64_t>(p, want_head);
    p = seastar::write_le<uint64_t>(p, head);
}

inline void tracepoint_grab_capacity_pending(uint64_t cap, uint64_t head) {
    auto p = reinterpret_cast<char*>(g_tracer.write(12 + 16));
    p = seastar::write_le<uint32_t>(p, static_cast<uint32_t>(trace_events::GRAB_CAPACITY_PENDING));
    p = seastar::write_le<uint64_t>(p, g_tracer.rdtsc());
    p = seastar::write_le<uint64_t>(p, cap);
    p = seastar::write_le<uint64_t>(p, head);
}

inline void tracepoint_replenish(uint64_t new_head) {
    tracepoint_unary<uint64_t>(trace_events::REPLENISH, new_head);
}

inline void tracepoint_dispatch_queue(uint8_t id) {
    tracepoint_unary<uint8_t>(trace_events::DISPATCH_QUEUE, id);
}

inline void tracepoint_dispatch_requests(uint64_t queued) {
    tracepoint_unary<uint64_t>(trace_events::DISPATCH_REQUESTS, queued);
}

inline void tracepoint_monitoring_scrape() {
    tracepoint_nullary(trace_events::MONITORING_SCRAPE);
}


} // namespace seastar
