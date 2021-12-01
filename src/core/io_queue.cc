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


#include <boost/intrusive/parent_from_member.hpp>
#include <seastar/core/file.hh>
#include <seastar/core/fair_queue.hh>
#include <seastar/core/io_queue.hh>
#include <seastar/core/io_intent.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/metrics.hh>
#include <seastar/core/linux-aio.hh>
#include <seastar/core/internal/io_desc.hh>
#include <seastar/core/internal/io_sink.hh>
#include <seastar/core/io_priority_class.hh>
#include <seastar/util/log.hh>
#include <chrono>
#include <mutex>
#include <array>
#include <fmt/format.h>
#include <fmt/ostream.h>

namespace seastar {

logger io_log("io");

using namespace std::chrono_literals;
using namespace internal::linux_abi;

struct default_io_exception_factory {
    static auto cancelled() {
        return cancelled_error();
    }
};

class io_queue::priority_class_data {
    const io_priority_class _pc;
    uint32_t _shares;
    struct {
        size_t bytes = 0;
        uint64_t ops = 0;

        void add(size_t len) noexcept {
            ops++;
            bytes += len;
        }
    } _rwstat[2] = {};
    uint32_t _nr_queued;
    uint32_t _nr_executing;
    std::chrono::duration<double> _queue_time;
    std::chrono::duration<double> _total_queue_time;
    std::chrono::duration<double> _total_execution_time;
    std::chrono::duration<double> _starvation_time;
    io_queue::clock_type::time_point _activated;
    metrics::metric_groups _metric_groups;

    void register_stats(sstring name, sstring mountpoint);

public:
    void rename(sstring new_name, sstring mountpoint);
    void update_shares(uint32_t shares) noexcept {
        _shares = std::max(shares, 1u);
    }

    priority_class_data(io_priority_class pc, uint32_t shares, sstring name, sstring mountpoint)
        : _pc(pc)
        , _shares(shares)
        , _nr_queued(0)
        , _nr_executing(0)
        , _queue_time(0)
        , _total_queue_time(0)
        , _total_execution_time(0)
        , _starvation_time(0)
    {
        register_stats(name, mountpoint);
    }

    void on_queue() noexcept {
        _nr_queued++;
        if (_nr_executing == 0 && _nr_queued == 1) {
            _activated = io_queue::clock_type::now();
        }
    }

    void on_dispatch(internal::io_direction_and_length dnl, std::chrono::duration<double> lat) noexcept {
        _rwstat[dnl.rw_idx()].add(dnl.length());
        _queue_time = lat;
        _total_queue_time += lat;
        _nr_queued--;
        _nr_executing++;
        if (_nr_executing == 1) {
            _starvation_time += io_queue::clock_type::now() - _activated;
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

    fair_queue::class_id fq_class() const noexcept { return _pc.id(); }
};

class io_desc_read_write final : public io_completion {
    io_queue& _ioq;
    io_queue::priority_class_data& _pclass;
    io_queue::clock_type::time_point _dispatched;
    const stream_id _stream;
    fair_queue_ticket _fq_ticket;
    promise<size_t> _pr;

public:
    io_desc_read_write(io_queue& ioq, io_queue::priority_class_data& pc, stream_id stream, fair_queue_ticket ticket)
        : _ioq(ioq)
        , _pclass(pc)
        , _stream(stream)
        , _fq_ticket(ticket)
    {}

    virtual void set_exception(std::exception_ptr eptr) noexcept override {
        io_log.trace("dev {} : req {} error", _ioq.dev_id(), fmt::ptr(this));
        _pclass.on_error();
        _ioq.complete_request(*this);
        _pr.set_exception(eptr);
        delete this;
    }

    virtual void complete(size_t res) noexcept override {
        io_log.trace("dev {} : req {} complete", _ioq.dev_id(), fmt::ptr(this));
        auto now = io_queue::clock_type::now();
        _pclass.on_complete(std::chrono::duration_cast<std::chrono::duration<double>>(now - _dispatched));
        _ioq.complete_request(*this);
        _pr.set_value(res);
        delete this;
    }

    void cancel() noexcept {
        _pclass.on_cancel();
        _pr.set_exception(std::make_exception_ptr(default_io_exception_factory::cancelled()));
        delete this;
    }

    void dispatch(internal::io_direction_and_length dnl, io_queue::clock_type::time_point queued) noexcept {
        auto now = io_queue::clock_type::now();
        _pclass.on_dispatch(dnl, std::chrono::duration_cast<std::chrono::duration<double>>(now - queued));
        _dispatched = now;
    }

    future<size_t> get_future() {
        return _pr.get_future();
    }

    fair_queue_ticket ticket() const noexcept { return _fq_ticket; }
    stream_id stream() const noexcept { return _stream; }
};

class queued_io_request : private internal::io_request {
    io_queue& _ioq;
    internal::io_direction_and_length _dnl;
    io_queue::clock_type::time_point _started;
    const stream_id _stream;
    fair_queue_entry _fq_entry;
    internal::cancellable_queue::link _intent;
    std::unique_ptr<io_desc_read_write> _desc;

    bool is_cancelled() const noexcept { return !_desc; }

public:
    queued_io_request(internal::io_request req, io_queue& q, io_queue::priority_class_data& pc, internal::io_direction_and_length dnl)
        : io_request(std::move(req))
        , _ioq(q)
        , _dnl(std::move(dnl))
        , _started(io_queue::clock_type::now())
        , _stream(_ioq.request_stream(_dnl))
        , _fq_entry(_ioq.request_fq_ticket(dnl))
        , _desc(std::make_unique<io_desc_read_write>(_ioq, pc, _stream, _fq_entry.ticket()))
    {
        io_log.trace("dev {} : req {} queue  len {} ticket {}", _ioq.dev_id(), fmt::ptr(&*_desc), _dnl.length(), _fq_entry.ticket());
    }

    queued_io_request(queued_io_request&&) = delete;

    void dispatch() noexcept {
        if (is_cancelled()) {
            _ioq.complete_cancelled_request(*this);
            delete this;
            return;
        }

        io_log.trace("dev {} : req {} submit", _ioq.dev_id(), fmt::ptr(&*_desc));
        _intent.maybe_dequeue();
        _desc->dispatch(_dnl, _started);
        _ioq.submit_request(_desc.release(), std::move(*this));
        delete this;
    }

    void cancel() noexcept {
        _ioq.cancel_request(*this);
        _desc.release()->cancel();
    }

    void set_intent(internal::cancellable_queue* cq) noexcept {
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

internal::cancellable_queue::cancellable_queue(cancellable_queue&& o) noexcept
        : _first(std::exchange(o._first, nullptr))
        , _rest(std::move(o._rest)) {
    if (_first != nullptr) {
        _first->_ref = this;
    }
}

internal::cancellable_queue& internal::cancellable_queue::operator=(cancellable_queue&& o) noexcept {
    if (this != &o) {
        _first = std::exchange(o._first, nullptr);
        _rest = std::move(o._rest);
        if (_first != nullptr) {
            _first->_ref = this;
        }
    }
    return *this;
}

internal::cancellable_queue::~cancellable_queue() {
    while (_first != nullptr) {
        queued_io_request::from_cq_link(*_first).cancel();
        pop_front();
    }
}

void internal::cancellable_queue::push_back(link& il) noexcept {
    if (_first == nullptr) {
        _first = &il;
        il._ref = this;
    } else {
        new (&il._hook) bi::slist_member_hook<>();
        _rest.push_back(il);
    }
}

void internal::cancellable_queue::pop_front() noexcept {
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

internal::intent_reference::intent_reference(io_intent* intent) noexcept : _intent(intent) {
    if (_intent != nullptr) {
        intent->_refs.bind(*this);
    }
}

io_intent* internal::intent_reference::retrieve() const {
    if (is_cancelled()) {
        throw default_io_exception_factory::cancelled();
    }

    return _intent;
}

void
io_queue::complete_request(io_desc_read_write& desc) noexcept {
    _requests_executing--;
    _streams[desc.stream()].notify_request_finished(desc.ticket());
}

fair_queue::config io_queue::make_fair_queue_config(const config& iocfg) {
    fair_queue::config cfg;
    cfg.ticket_weight_pace = iocfg.disk_us_per_request / read_request_base_count;
    cfg.ticket_size_pace = (iocfg.disk_us_per_byte * (1 << request_ticket_size_shift)) / read_request_base_count;
    return cfg;
}

io_queue::io_queue(io_group_ptr group, internal::io_sink& sink)
    : _priority_classes()
    , _group(std::move(group))
    , _sink(sink)
{
    auto fq_cfg = make_fair_queue_config(get_config());
    _streams.emplace_back(*_group->_fgs[0], fq_cfg);
    if (get_config().duplex) {
        _streams.emplace_back(*_group->_fgs[1], fq_cfg);
    }
    seastar_logger.debug("Created io queue, multipliers {}:{}",
            get_config().disk_req_write_to_read_multiplier,
            get_config().disk_bytes_write_to_read_multiplier);
}

fair_group::config io_group::make_fair_group_config(const io_queue::config& qcfg) noexcept {
    /*
     * It doesn't make sense to configure requests limit higher than
     * it can be if the queue is full of minimal requests. At the same
     * time setting too large value increases the chances to overflow
     * the group rovers and lock-up the queue.
     *
     * The same is technically true for bytes limit, but the group
     * rovers are configured in blocks (ticket size shift), and this
     * already makes a good protection.
     */
    auto max_req_count = std::min(qcfg.max_req_count,
        qcfg.max_bytes_count / io_queue::minimal_request_size);
    auto max_req_count_min = std::max(io_queue::read_request_base_count, qcfg.disk_req_write_to_read_multiplier);
    /*
     * Read requests weight read_request_base_count, writes weight
     * disk_req_write_to_read_multiplier. The fair queue limit must
     * be enough to pass the largest one through. The same is true
     * for request sizes, but that check is done run-time, see the
     * request_fq_ticket() method.
     */
    if (max_req_count < max_req_count_min) {
        seastar_logger.warn("The disk request rate is too low, configuring it to {}, but you may experience latency problems", max_req_count_min);
        max_req_count = max_req_count_min;
    }
    return fair_group::config(max_req_count,
        qcfg.max_bytes_count >> io_queue::request_ticket_size_shift);
}

io_group::io_group(io_queue::config io_cfg) noexcept
    : _config(std::move(io_cfg))
{
    auto fg_cfg = make_fair_group_config(_config);
    _fgs.push_back(std::make_unique<fair_group>(fg_cfg));
    if (_config.duplex) {
        _fgs.push_back(std::make_unique<fair_group>(fg_cfg));
    }
    seastar_logger.debug("Created io group, limits {}:{}", _config.max_req_count, _config.max_bytes_count);
}

io_queue::~io_queue() {
    // It is illegal to stop the I/O queue with pending requests.
    // Technically we would use a gate to guarantee that. But here, it is not
    // needed since this is expected to be destroyed only after the reactor is destroyed.
    //
    // And that will happen only when there are no more fibers to run. If we ever change
    // that, then this has to change.
    for (auto&& pc_data : _priority_classes) {
        if (pc_data) {
            for (auto&& s : _streams) {
                s.unregister_priority_class(pc_data->fq_class());
            }
        }
    }
}

std::mutex io_priority_class::_register_lock;
std::array<io_priority_class::class_info, io_priority_class::_max_classes> io_priority_class::_infos;

unsigned io_priority_class::get_shares() const {
    return _infos.at(_id).shares;
}

sstring io_priority_class::get_name() const {
    std::lock_guard<std::mutex> lock(_register_lock);
    return _infos.at(_id).name;
}

io_priority_class io_priority_class::register_one(sstring name, uint32_t shares) {
    std::lock_guard<std::mutex> lock(_register_lock);
    for (unsigned i = 0; i < _max_classes; ++i) {
        if (!_infos[i].registered()) {
            _infos[i].shares = shares;
            _infos[i].name = std::move(name);
        } else if (_infos[i].name != name) {
            continue;
        } else {
            // found an entry matching the name to be registered,
            // make sure it was registered with the same number shares
            // Note: those may change dynamically later on in the
            // fair queue
            assert(_infos[i].shares == shares);
        }
        return io_priority_class(i);
    }
    throw std::runtime_error("No more room for new I/O priority classes");
}

future<> io_priority_class::update_shares(uint32_t shares) const {
    // Keep registered shares intact, just update the ones
    // on reactor queues
    return engine().update_shares_for_queues(*this, shares);
}

bool io_priority_class::rename_registered(sstring new_name) {
    std::lock_guard<std::mutex> guard(_register_lock);
    for (unsigned i = 0; i < _max_classes; ++i) {
       if (!_infos[i].registered()) {
           break;
       }
       if (_infos[i].name == new_name) {
           if (i == id()) {
               return false;
           } else {
               io_log.error("trying to rename priority class with id {} to \"{}\" but that name already exists", id(), new_name);
               throw std::runtime_error(format("rename priority class: an attempt was made to rename a priority class to an"
                       " already existing name ({})", new_name));
           }
       }
    }
    _infos[id()].name = new_name;
    return true;
}

future<> io_priority_class::rename(sstring new_name) noexcept {
    return futurize_invoke([this, new_name = std::move(new_name)] () mutable {
        // Taking the lock here will prevent from newly registered classes
        // to register under the old name (and will prevent undefined
        // behavior since this array is shared cross shards. However, it
        // doesn't prevent the case where a newly registered class (that
        // got registered right after the lock release) will be unnecessarily
        // renamed. This is not a real problem and it is a lot better than
        // holding the lock until all cross shard activity is over.

        if (!rename_registered(new_name)) {
            return make_ready_future<>();
        }

        return smp::invoke_on_all([this, new_name = std::move(new_name)] {
            return engine().rename_queues(*this, new_name);
        });
    });
}

seastar::metrics::label io_queue_shard("ioshard");

void
io_queue::priority_class_data::rename(sstring new_name, sstring mountpoint) {
    try {
        register_stats(new_name, mountpoint);
    } catch (metrics::double_registration &e) {
        // we need to ignore this exception, since it can happen that
        // a class that was already created with the new name will be
        // renamed again (this will cause a double registration exception
        // to be thrown).
    }

}

void
io_queue::priority_class_data::register_stats(sstring name, sstring mountpoint) {
    shard_id owner = this_shard_id();
    seastar::metrics::metric_groups new_metrics;
    namespace sm = seastar::metrics;
    auto shard = sm::impl::shard();

    auto ioq_group = sm::label("mountpoint");
    auto mountlabel = ioq_group(mountpoint);

    auto class_label_type = sm::label("class");
    auto class_label = class_label_type(name);
    new_metrics.add_group("io_queue", {
            sm::make_derive("total_bytes", [this] {
                    return _rwstat[internal::io_direction_and_length::read_idx].bytes + _rwstat[internal::io_direction_and_length::write_idx].bytes;
                }, sm::description("Total bytes passed in the queue"), {io_queue_shard(shard), sm::shard_label(owner), mountlabel, class_label}),
            sm::make_derive("total_operations", [this] {
                    return _rwstat[internal::io_direction_and_length::read_idx].ops + _rwstat[internal::io_direction_and_length::write_idx].ops;
                }, sm::description("Total operations passed in the queue"), {io_queue_shard(shard), sm::shard_label(owner), mountlabel, class_label}),
            sm::make_derive("total_read_bytes", _rwstat[internal::io_direction_and_length::read_idx].bytes,
                    sm::description("Total read bytes passed in the queue"), {io_queue_shard(shard), sm::shard_label(owner), mountlabel, class_label}),
            sm::make_derive("total_read_ops", _rwstat[internal::io_direction_and_length::read_idx].ops,
                    sm::description("Total read operations passed in the queue"), {io_queue_shard(shard), sm::shard_label(owner), mountlabel, class_label}),
            sm::make_derive("total_write_bytes", _rwstat[internal::io_direction_and_length::write_idx].bytes,
                    sm::description("Total write bytes passed in the queue"), {io_queue_shard(shard), sm::shard_label(owner), mountlabel, class_label}),
            sm::make_derive("total_write_ops", _rwstat[internal::io_direction_and_length::write_idx].ops,
                    sm::description("Total write operations passed in the queue"), {io_queue_shard(shard), sm::shard_label(owner), mountlabel, class_label}),
            sm::make_derive("total_delay_sec", [this] {
                    return _total_queue_time.count();
                }, sm::description("Total time spent in the queue"), {io_queue_shard(shard), sm::shard_label(owner), mountlabel, class_label}),
            sm::make_derive("total_exec_sec", [this] {
                    return _total_execution_time.count();
                }, sm::description("Total time spent in disk"), {io_queue_shard(shard), sm::shard_label(owner), mountlabel, class_label}),
            sm::make_derive("starvation_time_sec", [this] {
                auto st = _starvation_time;
                if (_nr_queued != 0 && _nr_executing == 0) {
                    st += io_queue::clock_type::now() - _activated;
                }
                return st.count();
            }, sm::description("Total time spent starving for disk"), {io_queue_shard(shard), sm::shard_label(owner), mountlabel, class_label}),

            // Note: The counter below is not the same as reactor's queued-io-requests
            // queued-io-requests shows us how many requests in total exist in this I/O Queue.
            //
            // This counter lives in the priority class, so it will count only queued requests
            // that belong to that class.
            //
            // In other words: the new counter tells you how busy a class is, and the
            // old counter tells you how busy the system is.

            sm::make_queue_length("queue_length", _nr_queued, sm::description("Number of requests in the queue"), {io_queue_shard(shard), sm::shard_label(owner), mountlabel, class_label}),
            sm::make_queue_length("disk_queue_length", _nr_executing, sm::description("Number of requests in the disk"), {io_queue_shard(shard), sm::shard_label(owner), mountlabel, class_label}),
            sm::make_gauge("delay", [this] {
                return _queue_time.count();
            }, sm::description("random delay time in the queue"), {io_queue_shard(shard), sm::shard_label(owner), mountlabel, class_label}),
            sm::make_gauge("shares", _shares, sm::description("current amount of shares"), {io_queue_shard(shard), sm::shard_label(owner), mountlabel, class_label})
    });
    _metric_groups = std::exchange(new_metrics, {});
}

io_queue::priority_class_data& io_queue::find_or_create_class(const io_priority_class& pc) {
    auto id = pc.id();
    if (id >= _priority_classes.size()) {
        _priority_classes.resize(id + 1);
    }
    if (!_priority_classes[id]) {
        auto shares = pc.get_shares();
        auto name = pc.get_name();

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
        auto pc_data = std::make_unique<priority_class_data>(pc, shares, name, mountpoint());

        _priority_classes[id] = std::move(pc_data);
    }
    return *_priority_classes[id];
}

stream_id io_queue::request_stream(internal::io_direction_and_length dnl) const noexcept {
    return get_config().duplex ? dnl.rw_idx() : 0;
}

fair_queue_ticket io_queue::request_fq_ticket(internal::io_direction_and_length dnl) const noexcept {
    unsigned weight;
    size_t size;

    if (dnl.is_write()) {
        weight = get_config().disk_req_write_to_read_multiplier;
        size = get_config().disk_bytes_write_to_read_multiplier * dnl.length();
    } else {
        weight = io_queue::read_request_base_count;
        size = io_queue::read_request_base_count * dnl.length();
    }

    static thread_local size_t oversize_warning_threshold = 0;

    if (size >= get_config().max_bytes_count) {
        if (size > oversize_warning_threshold) {
            oversize_warning_threshold = size;
            io_log.warn("oversized request (length {}) submitted. "
                "dazed and confuzed, trimming its weight from {} down to {}", dnl.length(),
                size >> request_ticket_size_shift,
                get_config().max_bytes_count >> request_ticket_size_shift);
        }
        size = get_config().max_bytes_count;
    }

    return fair_queue_ticket(weight, size >> request_ticket_size_shift);
}

io_queue::request_limits io_queue::get_request_limits() const noexcept {
    request_limits l;
    l.max_read = align_down<size_t>(std::min<size_t>(get_config().disk_read_saturation_length, get_config().max_bytes_count / read_request_base_count), minimal_request_size);
    l.max_write = align_down<size_t>(std::min<size_t>(get_config().disk_write_saturation_length, get_config().max_bytes_count / get_config().disk_bytes_write_to_read_multiplier), minimal_request_size);
    return l;
}

future<size_t>
io_queue::queue_request(const io_priority_class& pc, size_t len, internal::io_request req, io_intent* intent) noexcept {
    return futurize_invoke([&pc, len, req = std::move(req), this, intent] () mutable {
        // First time will hit here, and then we create the class. It is important
        // that we create the shared pointer in the same shard it will be used at later.
        auto& pclass = find_or_create_class(pc);
        internal::io_direction_and_length dnl(req, len);
        auto queued_req = std::make_unique<queued_io_request>(std::move(req), *this, pclass, std::move(dnl));
        auto fut = queued_req->get_future();
        internal::cancellable_queue* cq = nullptr;
        if (intent != nullptr) {
            cq = &intent->find_or_create_cancellable_queue(dev_id(), pc.id());
        }

        _streams[queued_req->stream()].queue(pclass.fq_class(), queued_req->queue_entry());
        queued_req->set_intent(cq);
        queued_req.release();
        pclass.on_queue();
        _queued_requests++;
        return fut;
    });
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
    _sink.submit(desc, std::move(req));
}

void io_queue::cancel_request(queued_io_request& req) noexcept {
    _queued_requests--;
    _streams[req.stream()].notify_request_cancelled(req.queue_entry());
}

void io_queue::complete_cancelled_request(queued_io_request& req) noexcept {
    _streams[req.stream()].notify_request_finished(req.queue_entry().ticket());
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

future<>
io_queue::update_shares_for_class(const io_priority_class pc, size_t new_shares) {
    return futurize_invoke([this, pc, new_shares] {
        auto& pclass = find_or_create_class(pc);
        pclass.update_shares(new_shares);
        for (auto&& s : _streams) {
            s.update_shares_for_class(pclass.fq_class(), new_shares);
        }
    });
}

void
io_queue::rename_priority_class(io_priority_class pc, sstring new_name) {
    if (_priority_classes.size() > pc.id() &&
            _priority_classes[pc.id()]) {
        _priority_classes[pc.id()]->rename(new_name, get_config().mountpoint);
    }
}

void internal::io_sink::submit(io_completion* desc, internal::io_request req) noexcept {
    try {
        _pending_io.emplace_back(std::move(req), desc);
    } catch (...) {
        desc->set_exception(std::current_exception());
    }
}

}
