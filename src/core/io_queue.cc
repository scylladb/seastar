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

class priority_class_data {
    priority_class_ptr _ptr;
    size_t _bytes;
    uint64_t _ops;
    uint32_t _nr_queued;
    uint32_t _nr_executing;
    std::chrono::duration<double> _queue_time;
    std::chrono::duration<double> _total_queue_time;
    std::chrono::duration<double> _total_execution_time;
    metrics::metric_groups _metric_groups;

    void register_stats(sstring name, sstring mountpoint);

public:
    void rename(sstring new_name, sstring mountpoint);

    priority_class_data(sstring name, sstring mountpoint, priority_class_ptr ptr)
        : _ptr(ptr)
        , _bytes(0)
        , _ops(0)
        , _nr_queued(0)
        , _nr_executing(0)
        , _queue_time(0)
        , _total_queue_time(0)
        , _total_execution_time(0)
    {
        register_stats(name, mountpoint);
    }

    void on_queue() noexcept {
        _nr_queued++;
    }

    void on_dispatch(size_t len, std::chrono::duration<double> lat) noexcept {
        _ops++;
        _bytes += len;
        _queue_time = lat;
        _total_queue_time += lat;
        _nr_queued--;
        _nr_executing++;
    }

    void on_cancel() noexcept {
        _nr_queued--;
    }

    void on_complete(std::chrono::duration<double> lat) noexcept {
        _total_execution_time += lat;
        _nr_executing--;
    }

    void on_error() noexcept {
        _nr_executing--;
    }

    priority_class_ptr pclass() const noexcept { return _ptr; }
};

class io_desc_read_write final : public io_completion {
    io_queue& _ioq;
    priority_class_data& _pclass;
    std::chrono::steady_clock::time_point _dispatched;
    fair_queue_ticket _fq_ticket;
    promise<size_t> _pr;

public:
    io_desc_read_write(io_queue& ioq, priority_class_data& pc, fair_queue_ticket ticket)
        : _ioq(ioq)
        , _pclass(pc)
        , _fq_ticket(ticket)
    {}

    virtual void set_exception(std::exception_ptr eptr) noexcept override {
        io_log.trace("dev {} : req {} error", _ioq.dev_id(), fmt::ptr(this));
        _pclass.on_error();
        _ioq.notify_requests_finished(_fq_ticket);
        _pr.set_exception(eptr);
        delete this;
    }

    virtual void complete(size_t res) noexcept override {
        io_log.trace("dev {} : req {} complete", _ioq.dev_id(), fmt::ptr(this));
        auto now = std::chrono::steady_clock::now();
        _pclass.on_complete(std::chrono::duration_cast<std::chrono::duration<double>>(now - _dispatched));
        _ioq.notify_requests_finished(_fq_ticket);
        _pr.set_value(res);
        delete this;
    }

    void cancel() noexcept {
        _pclass.on_cancel();
        _pr.set_exception(std::make_exception_ptr(default_io_exception_factory::cancelled()));
        delete this;
    }

    void dispatch(size_t len, std::chrono::steady_clock::time_point queued) noexcept {
        auto now = std::chrono::steady_clock::now();
        _pclass.on_dispatch(len, std::chrono::duration_cast<std::chrono::duration<double>>(now - queued));
        _dispatched = now;
    }

    future<size_t> get_future() {
        return _pr.get_future();
    }
};

class queued_io_request : private internal::io_request {
    io_queue& _ioq;
    size_t _len;
    std::chrono::steady_clock::time_point _started;
    fair_queue_entry _fq_entry;
    internal::cancellable_queue::link _intent;
    std::unique_ptr<io_desc_read_write> _desc;

    bool is_cancelled() const noexcept { return !_desc; }

public:
    queued_io_request(internal::io_request req, io_queue& q, priority_class_data& pc, size_t l)
        : io_request(std::move(req))
        , _ioq(q)
        , _len(l)
        , _started(std::chrono::steady_clock::now())
        , _fq_entry(_ioq.request_fq_ticket(*this, _len))
        , _desc(std::make_unique<io_desc_read_write>(_ioq, pc, _fq_entry.ticket()))
    {
        io_log.trace("dev {} : req {} queue  len {} ticket {}", _ioq.dev_id(), fmt::ptr(&*_desc), _len, _fq_entry.ticket());
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
        _desc->dispatch(_len, _started);
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
io_queue::notify_requests_finished(fair_queue_ticket& desc) noexcept {
    _requests_executing--;
    _fq.notify_requests_finished(desc);
}

fair_queue::config io_queue::make_fair_queue_config(config iocfg) {
    fair_queue::config cfg;
    cfg.ticket_weight_pace = iocfg.disk_us_per_request / read_request_base_count;
    cfg.ticket_size_pace = (iocfg.disk_us_per_byte * (1 << request_ticket_size_shift)) / read_request_base_count;
    return cfg;
}

io_queue::io_queue(io_group_ptr group, internal::io_sink& sink, io_queue::config cfg)
    : _priority_classes()
    , _group(std::move(group))
    , _fq(_group->_fg, make_fair_queue_config(cfg))
    , _sink(sink)
    , _config(std::move(cfg)) {
    seastar_logger.debug("Created io queue, multipliers {}:{}",
            cfg.disk_req_write_to_read_multiplier,
            cfg.disk_bytes_write_to_read_multiplier);
}

fair_group::config io_group::make_fair_group_config(config iocfg) noexcept {
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
    auto max_req_count = std::min(iocfg.max_req_count,
        iocfg.max_bytes_count / io_queue::minimal_request_size);
    auto max_req_count_min = std::max(io_queue::read_request_base_count, iocfg.disk_req_write_to_read_multiplier);
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
        iocfg.max_bytes_count >> io_queue::request_ticket_size_shift);
}

io_group::io_group(config cfg) noexcept
    : _fg(make_fair_group_config(cfg))
    , _max_bytes_count(cfg.max_bytes_count) {
    seastar_logger.debug("Created io group, limits {}:{}", cfg.max_req_count, cfg.max_bytes_count);
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
            _fq.unregister_priority_class(pc_data->pclass());
        }
    }
}

std::mutex io_queue::_register_lock;
std::array<uint32_t, io_queue::_max_classes> io_queue::_registered_shares;
// We could very well just add the name to the io_priority_class. However, because that
// structure is passed along all the time - and sometimes we can't help but copy it, better keep
// it lean. The name won't really be used for anything other than monitoring.
std::array<sstring, io_queue::_max_classes> io_queue::_registered_names;

io_priority_class io_queue::register_one_priority_class(sstring name, uint32_t shares) {
    std::lock_guard<std::mutex> lock(_register_lock);
    for (unsigned i = 0; i < _max_classes; ++i) {
        if (!_registered_shares[i]) {
            _registered_shares[i] = shares;
            _registered_names[i] = std::move(name);
        } else if (_registered_names[i] != name) {
            continue;
        } else {
            // found an entry matching the name to be registered,
            // make sure it was registered with the same number shares
            // Note: those may change dynamically later on in the
            // fair queue priority_class_ptr
            assert(_registered_shares[i] == shares);
        }
        return io_priority_class(i);
    }
    throw std::runtime_error("No more room for new I/O priority classes");
}

bool io_queue::rename_one_priority_class(io_priority_class pc, sstring new_name) {
    std::lock_guard<std::mutex> guard(_register_lock);
    for (unsigned i = 0; i < _max_classes; ++i) {
       if (!_registered_shares[i]) {
           break;
       }
       if (_registered_names[i] == new_name) {
           if (i == pc.id()) {
               return false;
           } else {
               throw std::runtime_error(format("rename priority class: an attempt was made to rename a priority class to an"
                       " already existing name ({})", new_name));
           }
       }
    }
    _registered_names[pc.id()] = new_name;
    return true;
}

seastar::metrics::label io_queue_shard("ioshard");

void
priority_class_data::rename(sstring new_name, sstring mountpoint) {
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
priority_class_data::register_stats(sstring name, sstring mountpoint) {
    shard_id owner = this_shard_id();
    seastar::metrics::metric_groups new_metrics;
    namespace sm = seastar::metrics;
    auto shard = sm::impl::shard();

    auto ioq_group = sm::label("mountpoint");
    auto mountlabel = ioq_group(mountpoint);

    auto class_label_type = sm::label("class");
    auto class_label = class_label_type(name);
    new_metrics.add_group("io_queue", {
            sm::make_derive("total_bytes", _bytes, sm::description("Total bytes passed in the queue"), {io_queue_shard(shard), sm::shard_label(owner), mountlabel, class_label}),
            sm::make_derive("total_operations", _ops, sm::description("Total bytes passed in the queue"), {io_queue_shard(shard), sm::shard_label(owner), mountlabel, class_label}),
            sm::make_derive("total_delay_sec", [this] {
                    return _total_queue_time.count();
                }, sm::description("Total time spent in the queue"), {io_queue_shard(shard), sm::shard_label(owner), mountlabel, class_label}),
            sm::make_derive("total_exec_sec", [this] {
                    return _total_execution_time.count();
                }, sm::description("Total time spent in disk"), {io_queue_shard(shard), sm::shard_label(owner), mountlabel, class_label}),

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
            sm::make_gauge("shares", [this] {
                return this->_ptr->shares();
            }, sm::description("current amount of shares"), {io_queue_shard(shard), sm::shard_label(owner), mountlabel, class_label})
    });
    _metric_groups = std::exchange(new_metrics, {});
}

priority_class_data& io_queue::find_or_create_class(const io_priority_class& pc) {
    auto id = pc.id();
    if (id >= _priority_classes.size()) {
        _priority_classes.resize(id + 1);
    }
    if (!_priority_classes[id]) {
        auto shares = _registered_shares.at(id);
        sstring name;
        {
            std::lock_guard<std::mutex> lock(_register_lock);
            name = _registered_names.at(id);
        }

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
        auto pc_ptr = _fq.register_priority_class(shares);
        auto pc_data = std::make_unique<priority_class_data>(name, mountpoint(), pc_ptr);

        _priority_classes[id] = std::move(pc_data);
    }
    return *_priority_classes[id];
}

fair_queue_ticket io_queue::request_fq_ticket(const internal::io_request& req, size_t len) const {
    unsigned weight;
    size_t size;
    if (req.is_write()) {
        weight = _config.disk_req_write_to_read_multiplier;
        size = _config.disk_bytes_write_to_read_multiplier * len;
    } else if (req.is_read()) {
        weight = io_queue::read_request_base_count;
        size = io_queue::read_request_base_count * len;
    } else {
        throw std::runtime_error(fmt::format("Unrecognized request passing through I/O queue {}", req.opname()));
    }

    static thread_local size_t oversize_warning_threshold = 0;

    if (size >= _group->_max_bytes_count) {
        if (size > oversize_warning_threshold) {
            oversize_warning_threshold = size;
            io_log.warn("oversized request (length {}) submitted. "
                "dazed and confuzed, trimming its weight from {} down to {}", len,
                size >> request_ticket_size_shift,
                _group->_max_bytes_count >> request_ticket_size_shift);
        }
        size = _group->_max_bytes_count;
    }

    return fair_queue_ticket(weight, size >> request_ticket_size_shift);
}

io_queue::request_limits io_queue::get_request_limits() const noexcept {
    request_limits l;
    l.max_read = align_down<size_t>(std::min<size_t>(_config.disk_read_saturation_length, _group->_max_bytes_count / read_request_base_count), minimal_request_size);
    l.max_write = align_down<size_t>(std::min<size_t>(_config.disk_write_saturation_length, _group->_max_bytes_count / _config.disk_bytes_write_to_read_multiplier), minimal_request_size);
    return l;
}

future<size_t>
io_queue::queue_request(const io_priority_class& pc, size_t len, internal::io_request req, io_intent* intent) noexcept {
    return futurize_invoke([&pc, len, req = std::move(req), this, intent] () mutable {
        // First time will hit here, and then we create the class. It is important
        // that we create the shared pointer in the same shard it will be used at later.
        auto& pclass = find_or_create_class(pc);
        auto queued_req = std::make_unique<queued_io_request>(std::move(req), *this, pclass, len);
        auto fut = queued_req->get_future();
        internal::cancellable_queue* cq = nullptr;
        if (intent != nullptr) {
            cq = &intent->find_or_create_cancellable_queue(dev_id(), pc.id());
        }

        _fq.queue(pclass.pclass(), queued_req->queue_entry());
        queued_req->set_intent(cq);
        queued_req.release();
        pclass.on_queue();
        _queued_requests++;
        return fut;
    });
}

void io_queue::poll_io_queue() {
    _fq.dispatch_requests([] (fair_queue_entry& fqe) {
        queued_io_request::from_fq_entry(fqe).dispatch();
    });
}

void io_queue::submit_request(io_desc_read_write* desc, internal::io_request req) noexcept {
    _queued_requests--;
    _requests_executing++;
    _sink.submit(desc, std::move(req));
}

void io_queue::cancel_request(queued_io_request& req) noexcept {
    _queued_requests--;
    _fq.notify_request_cancelled(req.queue_entry());
}

void io_queue::complete_cancelled_request(queued_io_request& req) noexcept {
    _fq.notify_requests_finished(req.queue_entry().ticket());
}

future<>
io_queue::update_shares_for_class(const io_priority_class pc, size_t new_shares) {
    return futurize_invoke([this, pc, new_shares] {
        auto& pclass = find_or_create_class(pc);
        pclass.pclass()->update_shares(new_shares);
    });
}

void
io_queue::rename_priority_class(io_priority_class pc, sstring new_name) {
    if (_priority_classes.size() > pc.id() &&
            _priority_classes[pc.id()]) {
        _priority_classes[pc.id()]->rename(new_name, _config.mountpoint);
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
