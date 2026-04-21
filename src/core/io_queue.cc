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
#include <seastar/util/integrated-length.hh>

#include <seastar/core/file.hh>
#include <seastar/core/io_queue.hh>
#include <seastar/core/io_intent.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/when_all.hh>
#include <seastar/core/metrics.hh>
#include <seastar/core/internal/io_desc.hh>
#include <seastar/core/internal/io_sink.hh>
#include <seastar/core/io_priority_class.hh>
#include <seastar/core/internal/io_trace.hh>
#include <seastar/util/log.hh>

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

io_throttler::io_throttler(config cfg, unsigned /*nr_queues*/)
        : _token_bucket(fixed_point_factor,
                        std::max<capacity_t>(fixed_point_factor * token_bucket_t::rate_cast(cfg.rate_limit_duration).count(), tokens_capacity(cfg.limit_min_tokens)),
                        tokens_capacity(cfg.min_tokens),
                        true,
                        cfg.tau
                       )
{
    if (tokens_capacity(cfg.min_tokens) > _token_bucket.threshold()) {
        throw std::runtime_error("Fair-group replenisher limit is lower than threshold");
    }
}

void io_throttler::replenish_capacity(clock_type::time_point now) noexcept {
    _token_bucket.replenish(now);
}

struct io_group::priority_class_data {
    priority_class_group_data* parent;

    using token_bucket_t = internal::shared_token_bucket<uint64_t, std::ratio<1>, internal::capped_release::no>;

    static constexpr uint64_t bandwidth_burst_in_blocks = 10 << (20 - io_queue::block_size_shift); // 10MB
    static constexpr uint64_t bandwidth_threshold_in_blocks = 128 << (10 - io_queue::block_size_shift); // 128kB
    token_bucket_t tb;

    static uint64_t tokens(size_t length) noexcept {
        return length >> io_queue::block_size_shift;
    }

    void update_bandwidth(uint64_t bandwidth) {
        if (bandwidth >> io_queue::block_size_shift > tb.max_rate) {
            // It's ... tooooo big value indeed
            throw std::runtime_error(format("Too large rate, maximum is {}MB/s", tb.max_rate >> (20 - io_queue::block_size_shift)));
        }

        tb.update_rate(tokens(bandwidth));
    }

    priority_class_data(priority_class_group_data* p) noexcept
            : parent(p)
            , tb(std::numeric_limits<uint64_t>::max(), bandwidth_burst_in_blocks, bandwidth_threshold_in_blocks)
    {
    }
};

class io_queue::priority_entity {
protected:
    priority_entity* _parent;
    uint32_t _shares;
    bool _plugged{true};
    // Snapshot of _plugged taken at the start of a poll_io_queue() dispatch
    // pass.  Used by interior nodes (groups) to let sibling classes keep
    // dispatching even when one of them triggers a parent-level bw throttle
    // mid-pass.  Leaf classes consult their plug state live (see
    // priority_class_data::plugged_latched()).
    bool _plugged_latched{true};
    // Per-stream token pouches used for dispatch (leaf classes) or staging
    // before redistribution to children (interior nodes).
    boost::container::static_vector<io_throttler::tokens, 2> _tokens;
    // Children of this entity in the dispatch tree.  Leaf classes have an
    // empty vector; interior nodes (groups, shard root) list their children.
    std::vector<priority_entity*> _children;
    // Running sum of shares() over children that are active and plugged.
    // Serves as the denominator for proportional redistribution.
    uint32_t _active_children_shares{0};

    priority_entity(priority_entity* parent, uint32_t shares,
                    boost::container::static_vector<stream, 2>& streams)
            : _parent(parent)
            , _shares(std::max(shares, 1u))
    {
        for (auto& st : streams) {
            _tokens.emplace_back(st.out.token_bucket().limit());
        }
    }

    capacity_t drain_tokens(unsigned stream_idx) noexcept {
        return _tokens[stream_idx].drain();
    }

public:
    uint32_t shares() const noexcept { return _shares; }
    priority_entity* parent() const noexcept { return _parent; }

    bool plugged_self() const noexcept { return _plugged; }
    bool plugged_latched_self() const noexcept { return _plugged_latched; }
    void latch_plugged() noexcept { _plugged_latched = _plugged; }

    void update_shares(uint32_t shares) noexcept {
        _shares = std::max(shares, 1u);
    }

    bool try_consume(unsigned stream_idx, capacity_t cost) noexcept {
        return _tokens[stream_idx].try_consume(cost);
    }

    void add_tokens(unsigned stream_idx, capacity_t amount) noexcept {
        _tokens[stream_idx].refill(amount);
    }

    void add_child(priority_entity& child) noexcept {
        _children.push_back(&child);
    }

    void remove_child(priority_entity& child) noexcept {
        auto it = std::find(_children.begin(), _children.end(), &child);
        _children.erase(it);
    }

    virtual void child_started_dispatching(priority_entity& child) noexcept = 0;
    virtual void child_stopped_dispatching(priority_entity& child) noexcept = 0;
    virtual void child_shares_changed(priority_entity& child, uint32_t old_shares) noexcept = 0;

    // Drain token pouches and distribute proportionally to active+plugged
    // children.  Recursively calls redistribute() on children that have
    // their own sub-trees (groups).
    void redistribute() noexcept {
        if (_active_children_shares == 0) {
            return;
        }
        for (unsigned si = 0; si < _tokens.size(); si++) {
            auto available = drain_tokens(si);
            if (available == 0) {
                continue;
            }
            capacity_t distributed = 0;
            for (auto* child : _children) {
                if (child->is_dispatchable()) {
                    auto amount = capacity_t(double(available) * child->shares() / _active_children_shares);
                    child->add_tokens(si, amount);
                    distributed += amount;
                }
            }
            if (distributed < available) {
                add_tokens(si, available - distributed);
            }
        }
        for (auto* child : _children) {
            child->redistribute();
        }
    }

    virtual bool is_dispatchable() const noexcept = 0;

protected:
    ~priority_entity() = default;
};

struct io_queue::priority_class_group_data final : public io_queue::priority_entity {
    const unsigned _index;

    priority_class_group_data(unsigned index, uint32_t shares,
                              priority_entity& parent,
                              boost::container::static_vector<stream, 2>& streams)
        : priority_entity(&parent, shares, streams)
        , _index(index)
    {
        parent.add_child(*this);
    }

    void plug() noexcept {
        if (_plugged) {
            return;
        }
        _plugged = true;
        if (_active_children_shares > 0) {
            _parent->child_started_dispatching(*this);
        }
    }
    void unplug() noexcept {
        if (!_plugged) {
            return;
        }
        _plugged = false;
        if (_active_children_shares > 0) {
            _parent->child_stopped_dispatching(*this);
        }
    }

    void child_started_dispatching(priority_entity& child) noexcept override {
        if (_active_children_shares == 0 && _plugged) {
            _parent->child_started_dispatching(*this);
        }
        _active_children_shares += child.shares();
    }

    void child_stopped_dispatching(priority_entity& child) noexcept override {
        _active_children_shares -= child.shares();
        if (_active_children_shares == 0 && _plugged) {
            _parent->child_stopped_dispatching(*this);
        }
    }

    void child_shares_changed(priority_entity& child, uint32_t old_shares) noexcept override {
        _active_children_shares = _active_children_shares - old_shares + child.shares();
    }

    bool is_dispatchable() const noexcept override { return _active_children_shares > 0 && _plugged; }
};

class io_desc_read_write final : public io_completion {
    io_queue& _ioq;
    io_queue::priority_class_data& _pclass;
    io_queue::clock_type::time_point _ts;
    const stream_id _stream;
    const io_direction_and_length _dnl;
    const io_queue::capacity_t _fq_capacity;
    promise<size_t> _pr;
    iovec_keeper _iovs;
    uint64_t _dispatched_polls;

public:
    io_desc_read_write(io_queue& ioq, io_queue::priority_class_data& pc, stream_id stream, io_direction_and_length dnl, io_queue::capacity_t cap, iovec_keeper iovs)
        : _ioq(ioq)
        , _pclass(pc)
        , _ts(io_queue::clock_type::now())
        , _stream(stream)
        , _dnl(dnl)
        , _fq_capacity(cap)
        , _iovs(std::move(iovs))
    {
    }

    virtual void set_exception(std::exception_ptr eptr) noexcept override;
    virtual void complete(size_t res) noexcept override;
    void cancel() noexcept;
    void dispatch() noexcept;

    future<size_t> get_future() {
        return _pr.get_future();
    }

    io_queue::capacity_t capacity() const noexcept { return _fq_capacity; }
    stream_id stream() const noexcept { return _stream; }
    uint64_t polls() const noexcept { return _dispatched_polls; }
};

class queued_io_request : private internal::io_request {
    io_queue& _ioq;
    const stream_id _stream;
    io_queue::capacity_t _capacity;
    boost::intrusive::slist_member_hook<> _hook;
    internal::cancellable_queue::link _intent;
    std::unique_ptr<io_desc_read_write> _desc;

    bool is_cancelled() const noexcept { return !_desc; }

public:
    using list_t = boost::intrusive::slist<queued_io_request,
                       boost::intrusive::member_hook<queued_io_request,
                           boost::intrusive::slist_member_hook<>,
                           &queued_io_request::_hook>,
                       boost::intrusive::cache_last<true>,
                       boost::intrusive::constant_time_size<false>>;

    queued_io_request(internal::io_request req, io_queue& q, io_queue::capacity_t cap, io_queue::priority_class_data& pc, io_direction_and_length dnl, iovec_keeper iovs)
        : io_request(std::move(req))
        , _ioq(q)
        , _stream(_ioq.request_stream(dnl))
        , _capacity(cap)
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
    io_queue::capacity_t capacity() const noexcept { return _capacity; }
    void reset_capacity() noexcept { _capacity = 0; }
    stream_id stream() const noexcept { return _stream; }

    static queued_io_request& from_list_hook(boost::intrusive::slist_member_hook<>& h) noexcept {
        return *boost::intrusive::get_parent_from_member(&h, &queued_io_request::_hook);
    }

    static queued_io_request& from_cq_link(internal::cancellable_queue::link& link) noexcept {
        return *boost::intrusive::get_parent_from_member(&link, &queued_io_request::_intent);
    }
};

class io_queue::priority_class_data final : public io_queue::priority_entity {
    io_queue& _queue;
    const internal::priority_class _pc;
    struct {
        size_t bytes = 0;
        uint64_t ops = 0;

        void add(size_t len) noexcept {
            ops++;
            bytes += len;
        }
    } _rwstat[2] = {}, _splits = {};
    util::integrated_length<unsigned short, lowres_clock> _nr_queued;
    util::integrated_length<unsigned short, lowres_clock> _nr_executing;
    std::chrono::duration<double> _queue_time;
    std::chrono::duration<double> _total_queue_time;
    std::chrono::duration<double> _total_execution_time;
    std::chrono::duration<double> _starvation_time;
    io_queue::clock_type::time_point _activated;

    class bandwidth_throttler {
        io_group::priority_class_data::token_bucket_t& _tb;
        uint64_t _replenish_head{0};
        priority_class_data& _pc;
        std::optional<unsigned> _group;
        timer<lowres_clock> _replenish;

        void throttle() noexcept {
            if (_group) {
                _pc._queue.throttle_priority_class_group(*_group);
            } else {
                _pc._queue.throttle_priority_class(_pc);
            }
        }

        void unthrottle() noexcept {
            if (_group) {
                _pc._queue.unthrottle_priority_class_group(*_group);
            } else {
                _pc._queue.unthrottle_priority_class(_pc);
            }
        }

        void try_to_replenish() noexcept {
            _tb.replenish(io_queue::clock_type::now());
            auto delta = _tb.deficiency(_replenish_head);
            if (delta > 0) {
                _replenish.arm(std::chrono::duration_cast<std::chrono::microseconds>(_tb.duration_for(delta)));
            } else {
                unthrottle();
            }
        }

    public:
        bandwidth_throttler(io_group::priority_class_data& pg, priority_class_data& pc, std::optional<unsigned> g) noexcept
                : _tb(pg.tb)
                , _pc(pc)
                , _group(g)
                , _replenish([this] { try_to_replenish(); })
        {}

        void maybe_throttle(uint64_t ph) noexcept {
            if (_replenish.armed()) {
                return;
            }
            auto delta = _tb.deficiency(ph);
            if (delta == 0) {
                return;
            }
            auto dur = std::chrono::duration_cast<std::chrono::microseconds>(_tb.duration_for(delta));
            if (dur.count() <= 1) {
                // A sub-microsecond deficiency means the bucket's head just
                // hasn't been refreshed recently -- typical for classes with
                // no explicit bw cap, whose head stays put until the first
                // throttle event.  Replenish on the spot and retry instead
                // of going through a throttle/timer/unthrottle blip.
                // Safe to recurse: the tail (ph) doesn't move, and a single
                // replenish() advances the head enough to either clear the
                // deficiency or bring it above the sub-usec range.
                _tb.replenish(io_queue::clock_type::now());
                return maybe_throttle(ph);
            }
            throttle();
            _replenish.arm(dur);
        }

        void grab(uint64_t tokens) noexcept {
            auto ph = _tb.grab(tokens);
            // _replenish_head must track the latest grabbed head, not just
            // the head at the moment we first armed the timer.  Otherwise, if
            // more requests are dispatched for this class while the timer is
            // armed (which happens within a single poll pass, because the
            // plugged-latch keeps dispatch going until the next poll), the
            // timer fires for a stale, smaller head, sees deficiency == 0 and
            // unthrottles the class while the bucket is in fact still
            // deficient at the newest head.  The class then plugs back,
            // dispatches one more request, re-throttles -- a spurious
            // unthrottle/throttle round-trip and a systematic bandwidth
            // overshoot of one request per cycle.  Bucket tails are monotonic
            // (fetch_add-only) so plain assignment suffices.
            _replenish_head = ph;
            maybe_throttle(ph);
        }
    };

    boost::container::static_vector<bandwidth_throttler, 2> _bw;
    // Per-stream pending request queues.
    boost::container::static_vector<queued_io_request::list_t, 2> _queues;
    // Per-stream running total of capacity consumed by this class when its
    // requests succeed at try_consume() in the dispatch pass.  Exposed via
    // the "consumption" metric.
    boost::container::static_vector<capacity_t, 2> _consumption;

    void start_dispatching() noexcept {
        _parent->child_started_dispatching(*this);
    }

    void stop_dispatching() noexcept {
        _parent->child_stopped_dispatching(*this);
    }

    void activate() noexcept {
        if (_plugged) {
            start_dispatching();
        }
    }

    void deactivate() noexcept {
        if (_plugged) {
            stop_dispatching();
        }
    }

public:
    void plug() noexcept {
        _plugged = true;
        if (is_active()) {
            start_dispatching();
        }
    }
    void unplug() noexcept {
        _plugged = false;
        if (is_active()) {
            stop_dispatching();
        }
    }
    // Plugged means ready to dispatch: class itself is not throttled and
    // all ancestors up to the root are plugged.
    bool plugged() const noexcept {
        for (auto* e = static_cast<const priority_entity*>(this); e != nullptr; e = e->parent()) {
            if (!e->plugged_self()) {
                return false;
            }
        }
        return true;
    }

    // Latched version used during dispatch.  Only the parent (group) half
    // is actually latched -- the whole point of the latch is to let siblings
    // in a group finish their turn within a pass when one of them triggers a
    // parent-level throttle.  For the class itself there is no such concern
    // (classes don't share pouches), so we consult the live class-self plug.
    bool plugged_latched() const noexcept {
        if (!_plugged) {
            return false;
        }
        for (auto* e = _parent; e != nullptr; e = e->parent()) {
            if (!e->plugged_latched_self()) {
                return false;
            }
        }
        return true;
    }

    void update_shares(uint32_t shares) noexcept {
        uint32_t old_shares = _shares;
        priority_entity::update_shares(shares);
        if (_shares != old_shares && is_active() && _plugged) {
            _parent->child_shares_changed(*this, old_shares);
        }
    }

    bool is_active() const noexcept { return _nr_queued.value() > 0; }
    bool is_dispatchable() const noexcept override { return is_active() && _plugged; }

    void child_started_dispatching(priority_entity&) noexcept override { abort(); }
    void child_stopped_dispatching(priority_entity&) noexcept override { abort(); }
    void child_shares_changed(priority_entity&, uint32_t) noexcept override { abort(); }

    queued_io_request::list_t& queue(unsigned stream_idx) noexcept {
        return _queues[stream_idx];
    }

    capacity_t consumption(stream_id s) const noexcept { return _consumption[s]; }
    void account_consumption(stream_id s, capacity_t cost) noexcept { _consumption[s] += cost; }

    priority_class_data(internal::priority_class pc, uint32_t shares, io_queue& q,
                        io_group::priority_class_data& pg, std::optional<unsigned> group_index, priority_entity& parent)
        : priority_entity(&parent, shares, q._streams)
        , _queue(q)
        , _pc(pc)
        , _nr_queued(0)
        , _nr_executing(0)
        , _queue_time(0)
        , _total_queue_time(0)
        , _total_execution_time(0)
        , _starvation_time(0)
    {
        _bw.emplace_back(pg, *this, std::nullopt);
        if (pg.parent != nullptr) {
            _bw.emplace_back(*pg.parent, *this, group_index);
        }
        unsigned nr_streams = q._streams.size();
        for (unsigned i = 0; i < nr_streams; i++) {
            _queues.emplace_back();
            _consumption.emplace_back(0);
        }
        parent.add_child(*this);
    }
    priority_class_data(const priority_class_data&) = delete;
    priority_class_data(priority_class_data&&) = delete;

    ~priority_class_data() {
        SEASTAR_ASSERT(_nr_queued == 0);
        SEASTAR_ASSERT(_nr_executing == 0);
        _parent->remove_child(*this);
    }

    void on_queue() noexcept {
        if (_nr_queued++ == 0) {
            activate();
            if (_nr_executing == 0) {
                _activated = io_queue::clock_type::now();
            }
        }
    }

    void on_dispatch(io_direction_and_length dnl, std::chrono::duration<double> lat) noexcept {
        _rwstat[dnl.rw_idx()].add(dnl.length());
        _queue_time = lat;
        _total_queue_time += lat;
        if (--_nr_queued == 0) {
            deactivate();
        }
        _nr_executing++;
        if (_nr_executing == 1) {
            _starvation_time += io_queue::clock_type::now() - _activated;
        }

        auto tokens = io_group::priority_class_data::tokens(dnl.length());
        for (auto& bw : _bw) {
            bw.grab(tokens);
        }
    }

    void on_cancel() noexcept {
        if (--_nr_queued == 0) {
            deactivate();
        }
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

    void on_before_dispatch() noexcept {
        _nr_queued.checkpoint();
    }

    void on_after_dispatch() noexcept {
        _nr_executing.checkpoint();
    }

    std::vector<seastar::metrics::impl::metric_definition_impl> metrics();
    metrics::metric_groups metric_groups;
};

// The shard root entity sits at the top of the per-shard dispatch tree.
// It has no parent; when its first child becomes dispatchable it activates
// its consumers on the global token bucket, and deactivates them when the
// last child goes idle.
struct io_queue::dispatch_root final : public io_queue::priority_entity {
    boost::container::static_vector<io_throttler::consumer, 2> _consumers;

    dispatch_root(boost::container::static_vector<stream, 2>& streams)
        : priority_entity(nullptr, 1, streams)
    {
        for (auto& st : streams) {
            _consumers.emplace_back(st.out.token_bucket(), capacity_t(0));
        }
    }

    void child_started_dispatching(priority_entity& child) noexcept override {
        bool was_idle = (_active_children_shares == 0);
        _active_children_shares += child.shares();
        for (auto& c : _consumers) {
            c.update_shares(capacity_t(_active_children_shares));
            if (was_idle) {
                c.activate();
            }
        }
    }

    void child_stopped_dispatching(priority_entity& child) noexcept override {
        _active_children_shares -= child.shares();
        for (auto& c : _consumers) {
            if (_active_children_shares == 0) {
                c.deactivate();
            } else {
                c.update_shares(capacity_t(_active_children_shares));
            }
        }
    }

    void child_shares_changed(priority_entity& child, uint32_t old_shares) noexcept override {
        _active_children_shares = _active_children_shares - old_shares + child.shares();
        for (auto& c : _consumers) {
            c.update_shares(capacity_t(_active_children_shares));
        }
    }

    // Pull tokens from the global bucket into the root's pouches.
    void refill() noexcept {
        for (unsigned si = 0; si < _consumers.size(); si++) {
            _tokens[si].refill(_consumers[si].accrued());
        }
    }

    bool is_dispatchable() const noexcept override { return false; }
};

void io_desc_read_write::set_exception(std::exception_ptr eptr) noexcept {
    _pclass.on_error();
    _ioq.complete_request(*this, std::chrono::duration<double>(0.0));
    _pr.set_exception(eptr);
    delete this;
}

void io_desc_read_write::complete(size_t res) noexcept {
    auto now = io_queue::clock_type::now();
    auto delay = std::chrono::duration_cast<std::chrono::duration<double>>(now - _ts);
    _pclass.on_complete(delay);
    _ioq.complete_request(*this, delay);
    _pr.set_value(res);
    delete this;
}

void io_desc_read_write::cancel() noexcept {
    _pclass.on_cancel();
    _pr.set_exception(std::make_exception_ptr(default_io_exception_factory::cancelled()));
    delete this;
}

void io_desc_read_write::dispatch() noexcept {
    auto now = io_queue::clock_type::now();
    _pclass.on_dispatch(_dnl, std::chrono::duration_cast<std::chrono::duration<double>>(now - _ts));
    _ts = now;
    _dispatched_polls = engine().polls();
}

namespace internal {

priority_class::priority_class(const scheduling_group& sg) noexcept : _id(internal::scheduling_group_index(sg))
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

    io_log.error("Invalid operation for split: {}", static_cast<int>(op));
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

const io_throttler& get_throttler(const io_queue& ioq, unsigned stream) {
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

    if (delay > _stall_threshold) {
        _stall_threshold *= 2;
        io_log.warn("Request took {:.3f}ms ({} polls) to execute, queued {} executing {}",
            std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(delay).count(),
            engine().polls() - desc.polls(), _queued_requests, _requests_executing);
    }
}


io_queue::io_queue(io_group_ptr group, internal::io_sink& sink)
    : _priority_classes()
    , _group(std::move(group))
    , _id(_group->_config.id)
    , _sink(sink)
    , _averaging_decay_timer([this] {
        update_flow_ratio();
        lower_stall_threshold();
    })
    , _stall_threshold_min(std::max(get_config().stall_threshold, 1ms))
    , _stall_threshold(_stall_threshold_min)
    , _physical_block_size(get_config().physical_block_size)
{
    auto& cfg = get_config();
    if (cfg.duplex) {
        static_assert(internal::io_direction_and_length::write_idx == 0);
        _streams.emplace_back(_group->_fgs[0], "write");
        static_assert(internal::io_direction_and_length::read_idx == 1);
        _streams.emplace_back(_group->_fgs[1], "read");
    } else {
        _streams.emplace_back(_group->_fgs[0], "rw");
    }
    _root = std::make_unique<dispatch_root>(_streams);
    _averaging_decay_timer.arm_periodic(std::chrono::duration_cast<std::chrono::milliseconds>(_group->io_latency_goal() * cfg.averaging_decay_ticks));

    if (cfg.with_metrics) {
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
}

io_throttler::config io_group::configure_throttler(const io_queue::config& qcfg) noexcept {
    io_throttler::config cfg;
    cfg.label = fmt::format("io-queue-{}", qcfg.id);
    double min_weight = std::min(io_queue::read_request_base_count, qcfg.disk_req_write_to_read_multiplier);
    double min_size = std::min(io_queue::read_request_base_count, qcfg.disk_blocks_write_to_read_multiplier);
    cfg.min_tokens = min_weight / qcfg.req_count_rate + min_size / qcfg.blocks_count_rate;
    double limit_min_weight = std::max(io_queue::read_request_base_count, qcfg.disk_req_write_to_read_multiplier);
    double limit_min_size = std::max(io_queue::read_request_base_count, qcfg.disk_blocks_write_to_read_multiplier) * qcfg.block_count_limit_min;
    cfg.limit_min_tokens = limit_min_weight / qcfg.req_count_rate + limit_min_size / qcfg.blocks_count_rate;
    cfg.rate_limit_duration = qcfg.rate_limit_duration;
    cfg.tau = qcfg.tau;
    return cfg;
}

std::chrono::duration<double> io_group::io_latency_goal() const noexcept {
    return _fgs.front().rate_limit_duration();
}

io_group::io_group(io_queue::config io_cfg, unsigned nr_queues)
    : _config(std::move(io_cfg))
    , _allocated_on(this_shard_id())
{
    auto throttler_config = configure_throttler(_config);
    _fgs.emplace_back(throttler_config, nr_queues);
    if (_config.duplex) {
        _fgs.emplace_back(throttler_config, nr_queues);
    }

    auto goal = io_latency_goal();
    auto lvl = goal > 1.1 * _config.rate_limit_duration ? log_level::warn : log_level::debug;
    io_log.log(lvl, "IO queue uses {:.2f}ms latency goal for {}", goal.count() * 1000, _config.mountpoint);

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
            unsigned long req_length = 1ul << (shift + io_queue::block_size_shift);

            auto tokens = internal::request_tokens(io_direction_and_length(idx, req_length), _config);
            auto cap = fg.tokens_capacity(tokens);
            if (cap > max_cap) {
                if (shift == 0) {
                    throw std::runtime_error("IO-group limits are too low");
                }

                // The current shift will give us the biggest power of 2 request length for which
                // capacity is still less than max_cap, but request_length_limit might be a better
                // fit if tokens_capacity for that is closer to max_cap.
                auto fitting_req_length = 1ul << (shift - 1 + io_queue::block_size_shift);
                auto fitting_cap = fg.tokens_capacity(internal::request_tokens(io_direction_and_length(idx, fitting_req_length), _config));
                auto cap_limit = fg.tokens_capacity(internal::request_tokens(io_direction_and_length(idx, io_group::request_length_limit), _config));
                if (fitting_cap < cap_limit && cap_limit < max_cap) {
                    _max_request_length[idx] = io_group::request_length_limit;
                } else {
                    _max_request_length[idx] = fitting_req_length;
                }
                break;
            }

            // Break this loop if the calculated request length gets larger than the
            // request length limit. This avoids an infinite loop if the
            // configured io_properties are huge and capacity ends up being always 0.
            // _max_request_length will hold default values of io_group::request_length_limit
            if (req_length > io_group::request_length_limit) {
                io_log.info("IO queue was unable to find a suitable maximum request length, the search was cut-off early at: {}MB", io_group::request_length_limit >> 20);
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

            sm::make_queue_length("queue_length", [this] { return _nr_queued.value(); }, sm::description("Number of requests in the queue")),
            sm::make_queue_length("disk_queue_length", [this] { return _nr_executing.value(); }, sm::description("Number of requests in the disk")),
            sm::make_gauge("delay", [this] {
                return _queue_time.count();
            }, sm::description("random delay time in the queue")),
            sm::make_gauge("shares", _shares, sm::description("current amount of shares")),

            sm::make_counter("integrated_queue_length", [this] { return _nr_queued.integral(); }, sm::description("Integrated queue length")),
            sm::make_counter("integrated_disk_queue_length", [this] { return _nr_executing.integral(); }, sm::description("Integrated disk queue length")),
    });
}

void io_queue::register_stats(sstring name, priority_class_data& pc) {
    if (!get_config().with_metrics) {
        return;
    }

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

    for (stream_id si = 0; si < _streams.size(); si++) {
        auto& s = _streams[si];
        for (auto&& m : s.metrics(pc, si)) {
            m(owner_l)(mnt_l)(class_l)(group_l)(sm::label("stream")(s._label));
            metrics.emplace_back(std::move(m));
        }
    }

    new_metrics.add_group("io_queue", std::move(metrics));
    pc.metric_groups = std::exchange(new_metrics, {});
}

io_queue::priority_class_group_data& io_queue::find_or_create_class_group(unsigned index, uint32_t shares) {
    if (index >= _priority_groups.size()) {
        _priority_groups.resize(index + 1);
    }
    if (!_priority_groups[index]) {
        _priority_groups[index] = std::make_unique<priority_class_group_data>(index, shares, *_root, _streams);
    }
    return *_priority_groups[index];
}

io_queue::priority_class_data& io_queue::find_or_create_class(internal::priority_class pc) {
    auto id = pc.id();
    if (id >= _priority_classes.size()) {
        _priority_classes.resize(id + 1);
    }
    if (!_priority_classes[id]) {
        auto sg = internal::scheduling_group_from_index(id);
        auto ssg = internal::scheduling_supergroup_for(sg);

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

        priority_class_group_data* pcg = nullptr;
        std::optional<unsigned> group_index;
        if (!ssg.is_root()) {
            group_index = ssg.index();
            pcg = &find_or_create_class_group(*group_index, ssg.get_shares());
        }

        auto& pg = _group->find_or_create_class(pc, group_index);

        priority_entity& parent = pcg ? static_cast<priority_entity&>(*pcg) : *_root;
        auto shares = sg.get_shares();
        auto pc_data = std::make_unique<priority_class_data>(pc, shares, *this, pg, group_index, parent);
        register_stats(sg.name(), *pc_data);

        _priority_classes[id] = std::move(pc_data);
    }
    return *_priority_classes[id];
}

io_group::priority_class_group_data& io_group::find_or_create_class_group(unsigned group_index) {
    std::lock_guard _(_lock);
    return find_or_create_class_group_locked(group_index);
}

io_group::priority_class_group_data& io_group::find_or_create_class_group_locked(unsigned id) {
    if (id >= _priority_groups.size()) {
        _priority_groups.resize(id + 1);
    }
    if (!_priority_groups[id]) {
        auto pg = std::make_unique<priority_class_group_data>(nullptr);
        _priority_groups[id] = std::move(pg);
    }
    return *_priority_groups[id];
}

io_group::priority_class_data& io_group::find_or_create_class(internal::priority_class pc) {
    auto sg = internal::scheduling_group_from_index(pc.id());
    auto ssg = internal::scheduling_supergroup_for(sg);
    std::optional<unsigned> group_index;
    if (!ssg.is_root()) {
        group_index = ssg.index();
    }
    return find_or_create_class(pc, group_index);
}

io_group::priority_class_data& io_group::find_or_create_class(internal::priority_class pc, std::optional<unsigned> group_index) {
    std::lock_guard _(_lock);

    priority_class_group_data* parent = nullptr;
    if (group_index.has_value()) {
        parent = &find_or_create_class_group_locked(*group_index);
    }

    auto id = pc.id();
    if (id >= _priority_classes.size()) {
        _priority_classes.resize(id + 1);
    }
    if (!_priority_classes[id]) {
        auto pg = std::make_unique<priority_class_data>(parent);
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

io_queue::capacity_t io_queue::request_capacity(io_direction_and_length dnl) const noexcept {
    const auto& cfg = get_config();
    auto tokens = internal::request_tokens(dnl, cfg);
    if (_flow_ratio <= cfg.flow_ratio_backpressure_threshold) {
        return _streams[request_stream(dnl)].out.tokens_capacity(tokens);
    }

    auto stream = request_stream(dnl);
    auto cap = _streams[stream].out.tokens_capacity(tokens * _flow_ratio);
    auto max_cap = _streams[stream].out.maximum_capacity();
    return std::min(cap, max_cap);
}

io_queue::request_limits io_queue::get_request_limits() const noexcept {
    request_limits l;
    l.max_read = align_down<size_t>(std::min<size_t>(get_config().disk_read_saturation_length, _group->_max_request_length[io_direction_read]), 1 << block_size_shift);
    l.max_write = align_down<size_t>(std::min<size_t>(get_config().disk_write_saturation_length, _group->_max_request_length[io_direction_write]), 1 << block_size_shift);
    return l;
}

std::chrono::duration<double> io_queue::get_io_latency_goal() const noexcept {
    return _group->io_latency_goal();
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
            auto& cq = intent->find_or_create_cancellable_queue(_id, pc.id());
            queued_req->set_intent(cq);
        }

        auto stream_idx = queued_req->stream();
        pclass.queue(stream_idx).push_back(*queued_req.release());
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
        reactor::io_stats::local().aio_outsizes++;
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

future<size_t> io_queue::submit_io_read(size_t len, internal::io_request req, io_intent* intent, iovec_keeper iovs) noexcept {
    internal::priority_class pc = internal::priority_class(current_scheduling_group());
    auto& io_stats = reactor::io_stats::local();
    ++io_stats.aio_reads;
    io_stats.aio_read_bytes += len;
    return queue_request(std::move(pc), io_direction_and_length(io_direction_read, len), std::move(req), intent, std::move(iovs));
}

future<size_t> io_queue::submit_io_write(size_t len, internal::io_request req, io_intent* intent, iovec_keeper iovs) noexcept {
    internal::priority_class pc = internal::priority_class(current_scheduling_group());
    auto& io_stats = reactor::io_stats::local();
    ++io_stats.aio_writes;
    io_stats.aio_write_bytes += len;
    return queue_request(std::move(pc), io_direction_and_length(io_direction_write, len), std::move(req), intent, std::move(iovs));
}

// This function is called by the shard on every poll.
// It runs in two phases:
//   1. Replenish the global token bucket, flush publishers into the root's
//      consumers, then redistribute tokens down the tree proportionally.
//   2. Dispatch queued requests for every class, consuming from the pre-fetched token pool.

void io_queue::poll_io_queue() {
    for (auto& pc : _priority_classes) {
        if (pc) {
            pc->on_before_dispatch();
        }
    }

    auto now = clock_type::now();

    // Phase 1: replenish global bucket, pull tokens into root, redistribute
    // down the tree.
    for (auto& st : _streams) {
        st.out.replenish_capacity(now);
    }
    _root->refill();
    _root->redistribute();

    // Snapshot plugged state of groups before dispatch.  Once taken, a
    // group-level throttle event fired by one class's dispatch won't stop
    // sibling classes in the same group from dispatching within this same
    // pass; the latch is refreshed on the next poll_io_queue() call.
    // Classes themselves don't need a latch: see priority_class_data::
    // plugged_latched().
    for (auto& pg : _priority_groups) {
        if (pg) {
            pg->latch_plugged();
        }
    }

    // Phase 2: dispatch requests for each class using the pre-fetched tokens.
    for (unsigned si = 0; si < _streams.size(); si++) {
        for (auto& pc : _priority_classes) {
            if (!pc) {
                continue;
            }
            auto& q = pc->queue(si);
            if (q.empty() || !pc->plugged_latched()) {
                continue;
            }
            while (!q.empty() && pc->plugged_self()) {
                auto& req = q.front();
                auto cost = req.capacity();
                if (!pc->try_consume(si, cost)) {
                    break;
                }
                q.pop_front();
                pc->account_consumption(si, cost);
                req.dispatch();
            }
        }
    }

    for (auto& pc : _priority_classes) {
        if (pc) {
            pc->on_after_dispatch();
        }
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
    req.reset_capacity();
}

void io_queue::complete_cancelled_request(queued_io_request& req) noexcept {
}

io_queue::clock_type::time_point io_queue::next_pending_aio() const noexcept {
    for (const auto& pc : _priority_classes) {
        if (pc) {
            for (unsigned stream_idx = 0; stream_idx < _streams.size(); stream_idx++) {
                if (!pc->queue(stream_idx).empty()) {
                    return clock_type::now();
                }
            }
        }
    }
    return clock_type::time_point::max();
}

void
io_queue::update_shares_for_class(internal::priority_class pc, size_t new_shares) {
    auto& pclass = find_or_create_class(pc);
    pclass.update_shares(new_shares);
}


void io_queue::update_shares_for_class_group(unsigned index, size_t new_shares) {
    auto& pcg = find_or_create_class_group(index, new_shares);
    pcg.update_shares(new_shares);
}

future<> io_queue::update_bandwidth_for_class(internal::priority_class pc, uint64_t new_bandwidth) {
    return futurize_invoke([this, pc, new_bandwidth] {
        if (_group->_allocated_on == this_shard_id()) {
            auto& pclass = _group->find_or_create_class(pc);
            pclass.update_bandwidth(new_bandwidth);
            io_log.debug("Updated {} class bandwidth to {}MB/s", pc.id(), new_bandwidth >> 20);
        }
    });
}

future<> io_queue::update_bandwidth_for_class_group(unsigned group_index, uint64_t new_bandwidth) {
    return futurize_invoke([this, group_index, new_bandwidth] {
        if (_group->_allocated_on == this_shard_id()) {
            auto& pclass = _group->find_or_create_class_group(group_index);
            pclass.update_bandwidth(new_bandwidth);
            io_log.debug("Updated {} class group bandwidth to {}MB/s", group_index, new_bandwidth >> 20);
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

void io_queue::destroy_priority_class(internal::priority_class pc) noexcept {
    if (_priority_classes.size() > pc.id() && _priority_classes[pc.id()]) {
        _priority_classes[pc.id()].reset();
    }
}

void io_queue::throttle_priority_class(const priority_class_data& pc) noexcept {
    const_cast<priority_class_data&>(pc).unplug();
}

void io_queue::unthrottle_priority_class(const priority_class_data& pc) noexcept {
    const_cast<priority_class_data&>(pc).plug();
}

void io_queue::throttle_priority_class_group(unsigned group) noexcept {
    if (group < _priority_groups.size() && _priority_groups[group]) {
        _priority_groups[group]->unplug();
    }
}

void io_queue::unthrottle_priority_class_group(unsigned group) noexcept {
    if (group < _priority_groups.size() && _priority_groups[group]) {
        _priority_groups[group]->plug();
    }
}


std::vector<seastar::metrics::impl::metric_definition_impl> io_queue::stream::metrics(const priority_class_data& pc, stream_id si) {
    namespace sm = seastar::metrics;
    return std::vector<sm::impl::metric_definition_impl>({
            sm::make_counter("consumption",
                    [&pc, si] { return io_throttler::capacity_tokens(pc.consumption(si)); },
                    sm::description("Accumulated disk capacity units consumed by this class; an increment per-second rate indicates full utilization")),
            sm::make_counter("activations",
                    [] { return 0; },
                    sm::description("The number of times the class was woken up from idle")),
    });
}

} // seastar namespace
