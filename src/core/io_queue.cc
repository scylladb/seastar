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
 * Copyright 2019 ScyllaDB
 */

#ifdef SEASTAR_MODULE
module;
#endif

#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <utility>
#include <fmt/format.h>
#include <fmt/ostream.h>
#include <boost/intrusive/parent_from_member.hpp>
#include <boost/container/small_vector.hpp>
#include <sys/uio.h>
#include <seastar/util/assert.hh>

#ifdef SEASTAR_MODULE
module seastar;
#else
#include <seastar/core/file.hh>
#include <seastar/core/io_queue.hh>
#include <seastar/core/io_intent.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/when_all.hh>
#include <seastar/core/metrics.hh>
#include <seastar/core/internal/io_desc.hh>
#include <seastar/core/internal/io_sink.hh>
#include <seastar/core/io_priority_class.hh>
#include <seastar/util/log.hh>
#endif

namespace seastar {

logger io_log("io");

using namespace std::chrono_literals;
using io_direction_and_length = internal::io_direction_and_length;
static constexpr auto io_direction_read = io_direction_and_length::read_idx;
static constexpr auto io_direction_write = io_direction_and_length::write_idx;

struct default_io_exception_factory {
    static auto cancelled() {
        return cancelled_error();
    }
};

struct io_group::priority_class_data {
    using token_bucket_t = internal::shared_token_bucket<uint64_t, std::ratio<1>, internal::capped_release::no>;

    static constexpr uint64_t bandwidth_burst_in_blocks = 10 << (20 - io_queue::block_size_shift); // 10MB
    static constexpr uint64_t bandwidth_threshold_in_blocks = 128 << (10 - io_queue::block_size_shift); // 128kB
    token_bucket_t tb;

    uint64_t tokens(size_t length) const noexcept {
        return length >> io_queue::block_size_shift;
    }

    void update_bandwidth(uint64_t bandwidth) {
        if (bandwidth >> io_queue::block_size_shift > tb.max_rate) {
            // It's ... tooooo big value indeed
            throw std::runtime_error(format("Too large rate, maximum is {}MB/s", tb.max_rate >> (20 - io_queue::block_size_shift)));
        }

        tb.update_rate(tokens(bandwidth));
    }

    priority_class_data() noexcept
            : tb(std::numeric_limits<uint64_t>::max(), bandwidth_burst_in_blocks, bandwidth_threshold_in_blocks)
    {
    }
};

class io_queue::priority_class_data {
    io_queue& _queue;
    const internal::priority_class _pc;
    uint32_t _shares;
    struct {
        size_t bytes = 0;
        uint64_t ops = 0;

        void add(size_t len) noexcept {
            ops++;
            bytes += len;
        }
    } _rwstat[2] = {}, _splits = {};
    uint32_t _nr_queued;
    uint32_t _nr_executing;
    std::chrono::duration<double> _queue_time;
    std::chrono::duration<double> _total_queue_time;
    std::chrono::duration<double> _total_execution_time;
    std::chrono::duration<double> _starvation_time;
    io_queue::clock_type::time_point _activated;

    io_group::priority_class_data& _group;
    size_t _replenish_head;
    timer<lowres_clock> _replenish;

    void try_to_replenish() noexcept {
        _group.tb.replenish(io_queue::clock_type::now());
        auto delta = _group.tb.deficiency(_replenish_head);
        if (delta > 0) {
            _replenish.arm(std::chrono::duration_cast<std::chrono::microseconds>(_group.tb.duration_for(delta)));
        } else {
            _queue.unthrottle_priority_class(*this);
        }
    }

public:
    void update_shares(uint32_t shares) noexcept {
        _shares = std::max(shares, 1u);
    }

    void update_bandwidth(uint64_t bandwidth) {
        _group.update_bandwidth(bandwidth);
        io_log.debug("Updated {} class bandwidth to {}MB/s", _pc.id(), bandwidth >> 20);
    }

    priority_class_data(internal::priority_class pc, uint32_t shares, io_queue& q, io_group::priority_class_data& pg)
        : _queue(q)
        , _pc(pc)
        , _shares(shares)
        , _nr_queued(0)
        , _nr_executing(0)
        , _queue_time(0)
        , _total_queue_time(0)
        , _total_execution_time(0)
        , _starvation_time(0)
        , _group(pg)
        , _replenish([this] { try_to_replenish(); })
    {
    }
    priority_class_data(const priority_class_data&) = delete;
    priority_class_data(priority_class_data&&) = delete;

    void on_queue() noexcept {
        _nr_queued++;
        if (_nr_executing == 0 && _nr_queued == 1) {
            _activated = io_queue::clock_type::now();
        }
    }

    void on_dispatch(io_direction_and_length dnl, std::chrono::duration<double> lat) noexcept {
        _rwstat[dnl.rw_idx()].add(dnl.length());
        _queue_time = lat;
        _total_queue_time += lat;
        _nr_queued--;
        _nr_executing++;
        if (_nr_executing == 1) {
            _starvation_time += io_queue::clock_type::now() - _activated;
        }

        auto tokens = _group.tokens(dnl.length());
        auto ph = _group.tb.grab(tokens);
        auto delta = _group.tb.deficiency(ph);
        if (delta > 0) {
            _queue.throttle_priority_class(*this);
            _replenish_head = ph;
            _replenish.arm(std::chrono::duration_cast<std::chrono::microseconds>(_group.tb.duration_for(delta)));
        }
    }

    void on_cancel() noexcept {
        _nr_queued--;
    }

    void on_complete(std::chrono::duration<double> lat) noexcept {
        _total_execution_time += lat;
        _nr_executing--;
        if (_nr_executing == 0 && _nr_queued != 0) {
            _activated = io_queue::clock_type::now();
        }
    }

    void on_error() noexcept {
        _nr_executing--;
        if (_nr_executing == 0 && _nr_queued != 0) {
            _activated = io_queue::clock_type::now();
        }
    }

    void on_split(io_direction_and_length dnl) noexcept {
        _splits.add(dnl.length());
    }

    fair_queue::class_id fq_class() const noexcept { return _pc.id(); }

    std::vector<seastar::metrics::impl::metric_definition_impl> metrics();
    metrics::metric_groups metric_groups;
};

class io_desc_read_write final : public io_completion {
    io_queue& _ioq;
    io_queue::priority_class_data& _pclass;
    io_queue::clock_type::time_point _ts;
    const stream_id _stream;
    const io_direction_and_length _dnl;
    const fair_queue_entry::capacity_t _fq_capacity;
    promise<size_t> _pr;
    iovec_keeper _iovs;
    uint64_t _dispatched_polls;

public:
    io_desc_read_write(io_queue& ioq, io_queue::priority_class_data& pc, stream_id stream, io_direction_and_length dnl, fair_queue_entry::capacity_t cap, iovec_keeper iovs)
        : _ioq(ioq)
        , _pclass(pc)
        , _ts(io_queue::clock_type::now())
        , _stream(stream)
        , _dnl(dnl)
        , _fq_capacity(cap)
        , _iovs(std::move(iovs))
    {
        io_log.trace("dev {} : req {} queue  len {} capacity {}", _ioq.dev_id(), fmt::ptr(this), _dnl.length(), _fq_capacity);
    }

    virtual void set_exception(std::exception_ptr eptr) noexcept override {
        io_log.trace("dev {} : req {} error", _ioq.dev_id(), fmt::ptr(this));
        _pclass.on_error();
        _ioq.complete_request(*this, std::chrono::duration<double>(0.0));
        _pr.set_exception(eptr);
        delete this;
    }

    virtual void complete(size_t res) noexcept override {
        io_log.trace("dev {} : req {} complete", _ioq.dev_id(), fmt::ptr(this));
        auto now = io_queue::clock_type::now();
        auto delay = std::chrono::duration_cast<std::chrono::duration<double>>(now - _ts);
        _pclass.on_complete(delay);
        _ioq.complete_request(*this, delay);
        _pr.set_value(res);
        delete this;
    }

    void cancel() noexcept {
        _pclass.on_cancel();
        _pr.set_exception(std::make_exception_ptr(default_io_exception_factory::cancelled()));
        delete this;
    }

    void dispatch() noexcept {
        io_log.trace("dev {} : req {} submit", _ioq.dev_id(), fmt::ptr(this));
        auto now = io_queue::clock_type::now();
        _pclass.on_dispatch(_dnl, std::chrono::duration_cast<std::chrono::duration<double>>(now - _ts));
        _ts = now;
        _dispatched_polls = engine().polls();
    }

    future<size_t> get_future() {
        return _pr.get_future();
    }

    fair_queue_entry::capacity_t capacity() const noexcept { return _fq_capacity; }
    stream_id stream() const noexcept { return _stream; }
    uint64_t polls() const noexcept { return _dispatched_polls; }
};

class queued_io_request : private internal::io_request {
    io_queue& _ioq;
    const stream_id _stream;
    fair_queue_entry _fq_entry;
    internal::cancellable_queue::link _intent;
    std::unique_ptr<io_desc_read_write> _desc;

    bool is_cancelled() const noexcept { return !_desc; }

public:
    queued_io_request(internal::io_request req, io_queue& q, fair_queue_entry::capacity_t cap, io_queue::priority_class_data& pc, io_direction_and_length dnl, iovec_keeper iovs)
        : io_request(std::move(req))
        , _ioq(q)
        , _stream(_ioq.request_stream(dnl))
        , _fq_entry(cap)
        , _desc(std::make_unique<io_desc_read_write>(_ioq, pc, _stream, dnl, cap, std::move(iovs)))
    {
    }

    queued_io_request(queued_io_request&&) = delete;

    void dispatch() noexcept {
        if (is_cancelled()) {
            _ioq.complete_cancelled_request(*this);
            delete this;
            return;
        }

        _intent.maybe_dequeue();
        _desc->dispatch();
        _ioq.submit_request(_desc.release(), std::move(*this));
        delete this;
    }

    void cancel() noexcept {
        _ioq.cancel_request(*this);
        _desc.release()->cancel();
    }

    void set_intent(internal::cancellable_queue& cq) noexcept {
        _intent.enqueue(cq);
    }

    future<size_t> get_future() noexcept { return _desc->get_future(); }
    fair_queue_entry& queue_entry() noexcept { return _fq_entry; }
    stream_id stream() const noexcept { return _stream; }

    static queued_io_request& from_fq_entry(fair_queue_entry& ent) noexcept {
        return *boost::intrusive::get_parent_from_member(&ent, &queued_io_request::_fq_entry);
    }

    static queued_io_request& from_cq_link(internal::cancellable_queue::link& link) noexcept {
        return *boost::intrusive::get_parent_from_member(&link, &queued_io_request::_intent);
    }
};

namespace internal {

priority_class::priority_class(const scheduling_group& sg) noexcept : _id(internal::scheduling_group_index(sg))
{ }

priority_class::priority_class(internal::maybe_priority_class_ref pc) noexcept : priority_class(current_scheduling_group())
{ }

cancellable_queue::cancellable_queue(cancellable_queue&& o) noexcept
        : _first(std::exchange(o._first, nullptr))
        , _rest(std::move(o._rest)) {
    if (_first != nullptr) {
        _first->_ref = this;
    }
}

cancellable_queue& cancellable_queue::operator=(cancellable_queue&& o) noexcept {
    if (this != &o) {
        _first = std::exchange(o._first, nullptr);
        _rest = std::move(o._rest);
        if (_first != nullptr) {
            _first->_ref = this;
        }
    }
    return *this;
}

cancellable_queue::~cancellable_queue() {
    while (_first != nullptr) {
        queued_io_request::from_cq_link(*_first).cancel();
        pop_front();
    }
}

void cancellable_queue::push_back(link& il) noexcept {
    if (_first == nullptr) {
        _first = &il;
        il._ref = this;
    } else {
        new (&il._hook) bi::slist_member_hook<>();
        _rest.push_back(il);
    }
}

void cancellable_queue::pop_front() noexcept {
    _first->_ref = nullptr;
    if (_rest.empty()) {
        _first = nullptr;
    } else {
        _first = &_rest.front();
        _rest.pop_front();
        _first->_hook.~slist_member_hook<>();
        _first->_ref = this;
    }
}

intent_reference::intent_reference(io_intent* intent) noexcept : _intent(intent) {
    if (_intent != nullptr) {
        intent->_refs.bind(*this);
    }
}

io_intent* intent_reference::retrieve() const {
    if (is_cancelled()) {
        throw default_io_exception_factory::cancelled();
    }

    return _intent;
}

void io_sink::submit(io_completion* desc, io_request req) noexcept {
    try {
        _pending_io.emplace_back(std::move(req), desc);
    } catch (...) {
        desc->set_exception(std::current_exception());
    }
}

std::vector<io_request::part> io_request::split(size_t max_length) {
    auto op = opcode();
    if (op == operation::read || op == operation::write) {
        return split_buffer(max_length);
    }
    if (op == operation::readv || op == operation::writev) {
        return split_iovec(max_length);
    }

    seastar_logger.error("Invalid operation for split: {}", static_cast<int>(op));
    std::abort();
}

std::vector<io_request::part> io_request::split_buffer(size_t max_length) {
    std::vector<part> ret;
    // the layout of _read and _write should be identical, otherwise we need to
    // have two different implementations for each of them
    static_assert(std::is_same_v<decltype(_read), decltype(_write)>);
    const auto& op = _read;
    ret.reserve((op.size + max_length - 1) / max_length);

    size_t off = 0;
    do {
        size_t len = std::min(op.size - off, max_length);
        ret.push_back({ sub_req_buffer(off, len), len, {} });
        off += len;
    } while (off < op.size);

    return ret;
}

std::vector<io_request::part> io_request::split_iovec(size_t max_length) {
    std::vector<part> parts;
    std::vector<::iovec> vecs;
    // the layout of _readv and _writev should be identical, otherwise we need to
    // have two different implementations for each of them
    static_assert(std::is_same_v<decltype(_readv), decltype(_writev)>);
    const auto& op = _readv;
    ::iovec* cur = op.iovec;
    size_t pos = 0;
    size_t off = 0;
    ::iovec* end = cur + op.iov_len;
    size_t remaining = max_length;

    while (cur != end) {
        ::iovec iov;
        iov.iov_base = reinterpret_cast<char*>(cur->iov_base) + off;
        iov.iov_len = cur->iov_len - off;

        if (iov.iov_len <= remaining) {
            remaining -= iov.iov_len;
            vecs.push_back(std::move(iov));
            cur++;
            off = 0;
            continue;
        }

        if (remaining > 0) {
            iov.iov_len = remaining;
            off += remaining;
            vecs.push_back(std::move(iov));
        }

        auto req = sub_req_iovec(pos, vecs);
        parts.push_back({ std::move(req), max_length, std::move(vecs) });
        pos += max_length;
        remaining = max_length;
    }

    if (vecs.size() > 0) {
        SEASTAR_ASSERT(remaining < max_length);
        auto req = sub_req_iovec(pos, vecs);
        parts.push_back({ std::move(req), max_length - remaining, std::move(vecs) });
    }

    return parts;
}

sstring io_request::opname() const {
    switch (opcode()) {
    case io_request::operation::fdatasync:
        return "fdatasync";
    case io_request::operation::write:
        return "write";
    case io_request::operation::writev:
        return "vectored write";
    case io_request::operation::read:
        return "read";
    case io_request::operation::readv:
        return "vectored read";
    case io_request::operation::recv:
        return "recv";
    case io_request::operation::recvmsg:
        return "recvmsg";
    case io_request::operation::send:
        return "send";
    case io_request::operation::sendmsg:
        return "sendmsg";
    case io_request::operation::accept:
        return "accept";
    case io_request::operation::connect:
        return "connect";
    case io_request::operation::poll_add:
        return "poll add";
    case io_request::operation::poll_remove:
        return "poll remove";
    case io_request::operation::cancel:
        return "cancel";
    }
    std::abort();
}

const fair_group& get_fair_group(const io_queue& ioq, unsigned stream) {
    return ioq._group->_fgs[stream];
}

} // internal namespace

template <typename T>
void update_moving_average(T& result, T value, double factor) noexcept {
    result = result * factor + value * (1.0 - factor);
}

void io_queue::update_flow_ratio() noexcept {
    if (_requests_completed > _prev_completed) {
        auto instant = double(_requests_dispatched - _prev_dispatched) / double(_requests_completed - _prev_completed);
        update_moving_average(_flow_ratio, instant, get_config().flow_ratio_ema_factor);
        _prev_dispatched = _requests_dispatched;
        _prev_completed = _requests_completed;
    }
}

void io_queue::lower_stall_threshold() noexcept {
    auto new_threshold = _stall_threshold - std::chrono::milliseconds(1);
    _stall_threshold = std::max(_stall_threshold_min, new_threshold);
}

void
io_queue::complete_request(io_desc_read_write& desc, std::chrono::duration<double> delay) noexcept {
    _requests_executing--;
    _requests_completed++;
    _streams[desc.stream()].notify_request_finished(desc.capacity());

    if (delay > _stall_threshold) {
        _stall_threshold *= 2;
        io_log.warn("Request took {:.3f}ms ({} polls) to execute, queued {} executing {}",
            std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(delay).count(),
            engine().polls() - desc.polls(), _queued_requests, _requests_executing);
    }
}

fair_queue::config io_queue::make_fair_queue_config(const config& iocfg, sstring label) {
    fair_queue::config cfg;
    cfg.label = label;
    return cfg;
}

io_queue::io_queue(io_group_ptr group, internal::io_sink& sink)
    : _priority_classes()
    , _group(std::move(group))
    , _sink(sink)
    , _averaging_decay_timer([this] {
        update_flow_ratio();
        lower_stall_threshold();
    })
    , _stall_threshold_min(std::max(get_config().stall_threshold, 1ms))
    , _stall_threshold(_stall_threshold_min)
{
    auto& cfg = get_config();
    if (cfg.duplex) {
        static_assert(internal::io_direction_and_length::write_idx == 0);
        _streams.emplace_back(_group->_fgs[0], make_fair_queue_config(cfg, "write"));
        static_assert(internal::io_direction_and_length::read_idx == 1);
        _streams.emplace_back(_group->_fgs[1], make_fair_queue_config(cfg, "read"));
    } else {
        _streams.emplace_back(_group->_fgs[0], make_fair_queue_config(cfg, "rw"));
    }
    _averaging_decay_timer.arm_periodic(std::chrono::duration_cast<std::chrono::milliseconds>(_group->io_latency_goal() * cfg.averaging_decay_ticks));

    namespace sm = seastar::metrics;
    auto owner_l = sm::shard_label(this_shard_id());
    auto mnt_l = sm::label("mountpoint")(mountpoint());
    auto group_l = sm::label("iogroup")(to_sstring(_group->_allocated_on));
    _metric_groups.add_group("io_queue", {
        sm::make_gauge("flow_ratio", [this] { return _flow_ratio; },
                sm::description("Ratio of dispatch rate to completion rate. Is expected to be 1.0+ growing larger on reactor stalls or (!) disk problems"),
                { owner_l, mnt_l, group_l }),
    });
}

fair_group::config io_group::make_fair_group_config(const io_queue::config& qcfg) noexcept {
    fair_group::config cfg;
    cfg.label = fmt::format("io-queue-{}", qcfg.devid);
    double min_weight = std::min(io_queue::read_request_base_count, qcfg.disk_req_write_to_read_multiplier);
    double min_size = std::min(io_queue::read_request_base_count, qcfg.disk_blocks_write_to_read_multiplier);
    cfg.min_tokens = min_weight / qcfg.req_count_rate + min_size / qcfg.blocks_count_rate;
    double limit_min_weight = std::max(io_queue::read_request_base_count, qcfg.disk_req_write_to_read_multiplier);
    double limit_min_size = std::max(io_queue::read_request_base_count, qcfg.disk_blocks_write_to_read_multiplier) * qcfg.block_count_limit_min;
    cfg.limit_min_tokens = limit_min_weight / qcfg.req_count_rate + limit_min_size / qcfg.blocks_count_rate;
    cfg.rate_limit_duration = qcfg.rate_limit_duration;
    return cfg;
}

std::chrono::duration<double> io_group::io_latency_goal() const noexcept {
    return _fgs.front().rate_limit_duration();
}

io_group::io_group(io_queue::config io_cfg, unsigned nr_queues)
    : _config(std::move(io_cfg))
    , _allocated_on(this_shard_id())
{
    auto fg_cfg = make_fair_group_config(_config);
    _fgs.emplace_back(fg_cfg, nr_queues);
    if (_config.duplex) {
        _fgs.emplace_back(fg_cfg, nr_queues);
    }

    auto goal = io_latency_goal();
    auto lvl = goal > 1.1 * _config.rate_limit_duration ? log_level::warn : log_level::debug;
    seastar_logger.log(lvl, "IO queue uses {:.2f}ms latency goal for device {}", goal.count() * 1000, _config.devid);

    /*
     * The maximum request size shouldn't result in the capacity that would
     * be larger than the group's replenisher limit.
     *
     * To get the correct value check the 2^N sizes and find the largest one
     * with little enough capacity. Actually (FIXME) requests should calculate
     * capacities instead of request_fq_ticket() so this math would go away.
     */
    auto update_max_size = [this] (unsigned idx) {
        auto g_idx = _config.duplex ? idx : 0;
        const auto& fg = _fgs[g_idx];
        auto max_cap = fg.maximum_capacity();
        for (unsigned shift = 0; ; shift++) {
            auto tokens = internal::request_tokens(io_direction_and_length(idx, 1 << (shift + io_queue::block_size_shift)), _config);
            auto cap = fg.tokens_capacity(tokens);
            if (cap > max_cap) {
                if (shift == 0) {
                    throw std::runtime_error("IO-group limits are too low");
                }
                _max_request_length[idx] = 1 << ((shift - 1) + io_queue::block_size_shift);
                break;
            }
        };
    };

    update_max_size(io_direction_write);
    update_max_size(io_direction_read);
}

io_group::~io_group() {
}

io_queue::~io_queue() {
    // It is illegal to stop the I/O queue with pending requests.
    // Technically we would use a gate to guarantee that. But here, it is not
    // needed since this is expected to be destroyed only after the reactor is destroyed.
    //
    // And that will happen only when there are no more fibers to run. If we ever change
    // that, then this has to change.
    SEASTAR_ASSERT(_queued_requests == 0);
    for (auto&& pc_data : _priority_classes) {
        if (pc_data) {
            for (auto&& s : _streams) {
                s.unregister_priority_class(pc_data->fq_class());
            }
        }
    }
}

std::tuple<unsigned, sstring> get_class_info(io_priority_class_id pc) {
    auto sg = internal::scheduling_group_from_index(pc);
    return std::make_tuple(sg.get_shares(), sg.name());
}

std::vector<seastar::metrics::impl::metric_definition_impl> io_queue::priority_class_data::metrics() {
    namespace sm = seastar::metrics;
    return std::vector<sm::impl::metric_definition_impl>({
            sm::make_counter("total_bytes", [this] {
                    return _rwstat[io_direction_read].bytes + _rwstat[io_direction_write].bytes;
                }, sm::description("Total bytes passed in the queue")),
            sm::make_counter("total_operations", [this] {
                    return _rwstat[io_direction_read].ops + _rwstat[io_direction_write].ops;
                }, sm::description("Total operations passed in the queue")),
            sm::make_counter("total_read_bytes", _rwstat[io_direction_read].bytes,
                    sm::description("Total read bytes passed in the queue")),
            sm::make_counter("total_read_ops", _rwstat[io_direction_read].ops,
                    sm::description("Total read operations passed in the queue")),
            sm::make_counter("total_write_bytes", _rwstat[io_direction_write].bytes,
                    sm::description("Total write bytes passed in the queue")),
            sm::make_counter("total_write_ops", _rwstat[io_direction_write].ops,
                    sm::description("Total write operations passed in the queue")),
            sm::make_counter("total_split_ops", _splits.ops,
                    sm::description("Total number of requests split")),
            sm::make_counter("total_split_bytes", _splits.bytes,
                    sm::description("Total number of bytes split")),
            sm::make_counter("total_delay_sec", [this] {
                    return _total_queue_time.count();
                }, sm::description("Total time spent in the queue")),
            sm::make_counter("total_exec_sec", [this] {
                    return _total_execution_time.count();
                }, sm::description("Total time spent in disk")),
            sm::make_counter("starvation_time_sec", [this] {
                auto st = _starvation_time;
                if (_nr_queued != 0 && _nr_executing == 0) {
                    st += io_queue::clock_type::now() - _activated;
                }
                return st.count();
            }, sm::description("Total time spent starving for disk")),

            // Note: The counter below is not the same as reactor's queued-io-requests
            // queued-io-requests shows us how many requests in total exist in this I/O Queue.
            //
            // This counter lives in the priority class, so it will count only queued requests
            // that belong to that class.
            //
            // In other words: the new counter tells you how busy a class is, and the
            // old counter tells you how busy the system is.

            sm::make_queue_length("queue_length", _nr_queued, sm::description("Number of requests in the queue")),
            sm::make_queue_length("disk_queue_length", _nr_executing, sm::description("Number of requests in the disk")),
            sm::make_gauge("delay", [this] {
                return _queue_time.count();
            }, sm::description("random delay time in the queue")),
            sm::make_gauge("shares", _shares, sm::description("current amount of shares"))
    });
}

void io_queue::register_stats(sstring name, priority_class_data& pc) {
    namespace sm = seastar::metrics;
    seastar::metrics::metric_groups new_metrics;

    auto owner_l = sm::shard_label(this_shard_id());
    auto mnt_l = sm::label("mountpoint")(mountpoint());
    auto class_l = sm::label("class")(name);
    auto group_l = sm::label("iogroup")(to_sstring(_group->_allocated_on));

    std::vector<sm::metric_definition> metrics;
    for (auto&& m : pc.metrics()) {
        m(owner_l)(mnt_l)(class_l)(group_l);
        metrics.emplace_back(std::move(m));
    }

    for (auto&& s : _streams) {
        for (auto&& m : s.metrics(pc.fq_class())) {
            m(owner_l)(mnt_l)(class_l)(group_l)(sm::label("stream")(s.label()));
            metrics.emplace_back(std::move(m));
        }
    }

    new_metrics.add_group("io_queue", std::move(metrics));
    pc.metric_groups = std::exchange(new_metrics, {});
}

io_queue::priority_class_data& io_queue::find_or_create_class(internal::priority_class pc) {
    auto id = pc.id();
    if (id >= _priority_classes.size()) {
        _priority_classes.resize(id + 1);
    }
    if (!_priority_classes[id]) {
        auto [ shares, name ] = get_class_info(pc.id());

        // A note on naming:
        //
        // We could just add the owner as the instance id and have something like:
        //  io_queue-<class_owner>-<counter>-<class_name>
        //
        // However, when there are more than one shard per I/O queue, it is very useful
        // to know which shards are being served by the same queue. Therefore, a better name
        // scheme is:
        //
        //  io_queue-<queue_owner>-<counter>-<class_name>, shard=<class_owner>
        //  using the shard label to hold the owner number
        //
        // This conveys all the information we need and allows one to easily group all classes from
        // the same I/O queue (by filtering by shard)
        for (auto&& s : _streams) {
            s.register_priority_class(id, shares);
        }
        auto& pg = _group->find_or_create_class(pc);
        auto pc_data = std::make_unique<priority_class_data>(pc, shares, *this, pg);
        register_stats(name, *pc_data);

        _priority_classes[id] = std::move(pc_data);
    }
    return *_priority_classes[id];
}

io_group::priority_class_data& io_group::find_or_create_class(internal::priority_class pc) {
    std::lock_guard _(_lock);

    auto id = pc.id();
    if (id >= _priority_classes.size()) {
        _priority_classes.resize(id + 1);
    }
    if (!_priority_classes[id]) {
        auto pg = std::make_unique<priority_class_data>();
        _priority_classes[id] = std::move(pg);
    }

    return *_priority_classes[id];
}

stream_id io_queue::request_stream(io_direction_and_length dnl) const noexcept {
    return get_config().duplex ? dnl.rw_idx() : 0;
}

double internal::request_tokens(io_direction_and_length dnl, const io_queue::config& cfg) noexcept {
    struct {
        unsigned weight;
        unsigned size;
    } mult[2];

    mult[io_direction_write] = {
        cfg.disk_req_write_to_read_multiplier,
        cfg.disk_blocks_write_to_read_multiplier,
    };
    mult[io_direction_read] = {
        io_queue::read_request_base_count,
        io_queue::read_request_base_count,
    };

    const auto& m = mult[dnl.rw_idx()];

    return double(m.weight) / cfg.req_count_rate + double(m.size) * (dnl.length() >> io_queue::block_size_shift) / cfg.blocks_count_rate;
}

fair_queue_entry::capacity_t io_queue::request_capacity(io_direction_and_length dnl) const noexcept {
    const auto& cfg = get_config();
    auto tokens = internal::request_tokens(dnl, cfg);
    if (_flow_ratio <= cfg.flow_ratio_backpressure_threshold) {
        return _streams[request_stream(dnl)].tokens_capacity(tokens);
    }

    auto stream = request_stream(dnl);
    auto cap = _streams[stream].tokens_capacity(tokens * _flow_ratio);
    auto max_cap = _streams[stream].maximum_capacity();
    return std::min(cap, max_cap);
}

io_queue::request_limits io_queue::get_request_limits() const noexcept {
    request_limits l;
    l.max_read = align_down<size_t>(std::min<size_t>(get_config().disk_read_saturation_length, _group->_max_request_length[io_direction_read]), 1 << block_size_shift);
    l.max_write = align_down<size_t>(std::min<size_t>(get_config().disk_write_saturation_length, _group->_max_request_length[io_direction_write]), 1 << block_size_shift);
    return l;
}

future<size_t> io_queue::queue_one_request(internal::priority_class pc, io_direction_and_length dnl, internal::io_request req, io_intent* intent, iovec_keeper iovs) noexcept {
    return futurize_invoke([pc = std::move(pc), dnl = std::move(dnl), req = std::move(req), this, intent, iovs = std::move(iovs)] () mutable {
        // First time will hit here, and then we create the class. It is important
        // that we create the shared pointer in the same shard it will be used at later.
        auto& pclass = find_or_create_class(pc);
        auto cap = request_capacity(dnl);
        auto queued_req = std::make_unique<queued_io_request>(std::move(req), *this, cap, pclass, std::move(dnl), std::move(iovs));
        auto fut = queued_req->get_future();
        if (intent != nullptr) {
            auto& cq = intent->find_or_create_cancellable_queue(dev_id(), pc.id());
            queued_req->set_intent(cq);
        }

        _streams[queued_req->stream()].queue(pclass.fq_class(), queued_req->queue_entry());
        queued_req.release();
        pclass.on_queue();
        _queued_requests++;
        return fut;
    });
}

future<size_t> io_queue::queue_request(internal::priority_class pc, io_direction_and_length dnl, internal::io_request req, io_intent* intent, iovec_keeper iovs) noexcept {
    size_t max_length = _group->_max_request_length[dnl.rw_idx()];

    if (__builtin_expect(dnl.length() <= max_length, true)) {
        return queue_one_request(std::move(pc), dnl, std::move(req), intent, std::move(iovs));
    }

    std::vector<internal::io_request::part> parts;
    lw_shared_ptr<std::vector<future<size_t>>> p;

    try {
        parts = req.split(max_length);
        p = make_lw_shared<std::vector<future<size_t>>>();
        p->reserve(parts.size());
        find_or_create_class(pc).on_split(dnl);
        engine()._io_stats.aio_outsizes++;
    } catch (...) {
        return current_exception_as_future<size_t>();
    }

    // No exceptions from now on. If queue_one_request fails it will resolve
    // into exceptional future which will be picked up by when_all() below
    for (auto&& part : parts) {
        auto f = queue_one_request(pc, io_direction_and_length(dnl.rw_idx(), part.size), std::move(part.req), intent, std::move(part.iovecs));
        p->push_back(std::move(f));
    }

    return when_all(p->begin(), p->end()).then([p, max_length] (auto results) {
        bool prev_ok = true;
        size_t total = 0;
        std::exception_ptr ex;

        for (auto&& res : results) {
            if (!res.failed()) {
                if (prev_ok) {
                    size_t sz = res.get();
                    total += sz;
                    prev_ok &= (sz == max_length);
                }
            } else {
                if (!ex) {
                    ex = res.get_exception();
                } else {
                    res.ignore_ready_future();
                }
                prev_ok = false;
            }
        }

        if (total > 0) {
            return make_ready_future<size_t>(total);
        } else if (ex) {
            return make_exception_future<size_t>(std::move(ex));
        } else {
            return make_ready_future<size_t>(0);
        }
    });
}

future<size_t> io_queue::submit_io_read(internal::priority_class pc, size_t len, internal::io_request req, io_intent* intent, iovec_keeper iovs) noexcept {
    auto& r = engine();
    ++r._io_stats.aio_reads;
    r._io_stats.aio_read_bytes += len;
    return queue_request(std::move(pc), io_direction_and_length(io_direction_read, len), std::move(req), intent, std::move(iovs));
}

future<size_t> io_queue::submit_io_write(internal::priority_class pc, size_t len, internal::io_request req, io_intent* intent, iovec_keeper iovs) noexcept {
    auto& r = engine();
    ++r._io_stats.aio_writes;
    r._io_stats.aio_write_bytes += len;
    return queue_request(std::move(pc), io_direction_and_length(io_direction_write, len), std::move(req), intent, std::move(iovs));
}

void io_queue::poll_io_queue() {
    for (auto&& st : _streams) {
        st.dispatch_requests([] (fair_queue_entry& fqe) {
            queued_io_request::from_fq_entry(fqe).dispatch();
        });
    }
}

void io_queue::submit_request(io_desc_read_write* desc, internal::io_request req) noexcept {
    _queued_requests--;
    _requests_executing++;
    _requests_dispatched++;
    _sink.submit(desc, std::move(req));
}

void io_queue::cancel_request(queued_io_request& req) noexcept {
    _queued_requests--;
    _streams[req.stream()].notify_request_cancelled(req.queue_entry());
}

void io_queue::complete_cancelled_request(queued_io_request& req) noexcept {
    _streams[req.stream()].notify_request_finished(req.queue_entry().capacity());
}

io_queue::clock_type::time_point io_queue::next_pending_aio() const noexcept {
    clock_type::time_point next = clock_type::time_point::max();

    for (const auto& s : _streams) {
        clock_type::time_point n = s.next_pending_aio();
        if (n < next) {
            next = std::move(n);
        }
    }

    return next;
}

void
io_queue::update_shares_for_class(internal::priority_class pc, size_t new_shares) {
    auto& pclass = find_or_create_class(pc);
    pclass.update_shares(new_shares);
    for (auto&& s : _streams) {
        s.update_shares_for_class(pclass.fq_class(), new_shares);
    }
}

future<> io_queue::update_bandwidth_for_class(internal::priority_class pc, uint64_t new_bandwidth) {
    return futurize_invoke([this, pc, new_bandwidth] {
        if (_group->_allocated_on == this_shard_id()) {
            auto& pclass = find_or_create_class(pc);
            pclass.update_bandwidth(new_bandwidth);
        }
    });
}

void
io_queue::rename_priority_class(internal::priority_class pc, sstring new_name) {
    if (_priority_classes.size() > pc.id() &&
            _priority_classes[pc.id()]) {
        try {
            register_stats(new_name, *_priority_classes[pc.id()]);
        } catch (metrics::double_registration &e) {
            // we need to ignore this exception, since it can happen that
            // a class that was already created with the new name will be
            // renamed again (this will cause a double registration exception
            // to be thrown).
        }
    }
}

void io_queue::throttle_priority_class(const priority_class_data& pc) noexcept {
    for (auto&& s : _streams) {
        s.unplug_class(pc.fq_class());
    }
}

void io_queue::unthrottle_priority_class(const priority_class_data& pc) noexcept {
    for (auto&& s : _streams) {
        s.plug_class(pc.fq_class());
    }
}

} // seastar namespace
