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


#include <seastar/core/future-util.hh>
#include <seastar/core/file.hh>
#include <seastar/core/fair_queue.hh>
#include <seastar/core/io_queue.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/metrics.hh>
#include <seastar/core/linux-aio.hh>
#include <chrono>
#include <mutex>
#include <array>
#include "core/io_desc.hh"

namespace seastar {

using namespace std::chrono_literals;
using namespace internal::linux_abi;

class io_desc_read_write final : public io_desc {
    io_queue* _ioq_ptr;
    fair_queue_request_descriptor _fq_desc;
private:
    void notify_requests_finished() {
        _ioq_ptr->notify_requests_finished(_fq_desc);
    }
public:
    io_desc_read_write(io_queue* ioq, unsigned weight, unsigned size)
        : _ioq_ptr(ioq)
        , _fq_desc(fair_queue_request_descriptor{weight, size})
    {}

    fair_queue_request_descriptor& fq_descriptor() {
        return _fq_desc;
    }

    virtual void set_exception(std::exception_ptr eptr) {
        notify_requests_finished();
        io_desc::set_exception(std::move(eptr));
    }

    void set_value(io_event& ev) {
        notify_requests_finished();
        io_desc::set_value(ev);
    }
};


fair_queue::config io_queue::make_fair_queue_config(config iocfg) {
    fair_queue::config cfg;
    cfg.capacity = std::min(iocfg.capacity, reactor::max_aio_per_queue);
    cfg.max_req_count = iocfg.max_req_count;
    cfg.max_bytes_count = iocfg.max_bytes_count;
    return cfg;
}

io_queue::io_queue(io_queue::config cfg)
    : _priority_classes()
    , _fq(make_fair_queue_config(cfg))
    , _config(std::move(cfg)) {
}

io_queue::~io_queue() {
    // It is illegal to stop the I/O queue with pending requests.
    // Technically we would use a gate to guarantee that. But here, it is not
    // needed since this is expected to be destroyed only after the reactor is destroyed.
    //
    // And that will happen only when there are no more fibers to run. If we ever change
    // that, then this has to change.
    for (auto&& pc_vec : _priority_classes) {
        for (auto&& pc_data : pc_vec) {
            if (pc_data) {
                _fq.unregister_priority_class(pc_data->ptr);
            }
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

seastar::metrics::label io_queue_shard("ioshard");

io_queue::priority_class_data::priority_class_data(sstring name, sstring mountpoint, priority_class_ptr ptr, shard_id owner)
    : ptr(ptr)
    , bytes(0)
    , ops(0)
    , nr_queued(0)
    , queue_time(1s)
{
    register_stats(name, mountpoint, owner);
}

void
io_queue::priority_class_data::rename(sstring new_name, sstring mountpoint, shard_id owner) {
    try {
        register_stats(new_name, mountpoint, owner);
    } catch (metrics::double_registration &e) {
        // we need to ignore this exception, since it can happen that
        // a class that was already created with the new name will be
        // renamed again (this will cause a double registration exception
        // to be thrown).
    }

}

void
io_queue::priority_class_data::register_stats(sstring name, sstring mountpoint, shard_id owner) {
    seastar::metrics::metric_groups new_metrics;
    namespace sm = seastar::metrics;
    auto shard = sm::impl::shard();

    auto ioq_group = sm::label("mountpoint");
    auto mountlabel = ioq_group(mountpoint);

    auto class_label_type = sm::label("class");
    auto class_label = class_label_type(name);
    new_metrics.add_group("io_queue", {
            sm::make_derive("total_bytes", bytes, sm::description("Total bytes passed in the queue"), {io_queue_shard(shard), sm::shard_label(owner), mountlabel, class_label}),
            sm::make_derive("total_operations", ops, sm::description("Total bytes passed in the queue"), {io_queue_shard(shard), sm::shard_label(owner), mountlabel, class_label}),
            // Note: The counter below is not the same as reactor's queued-io-requests
            // queued-io-requests shows us how many requests in total exist in this I/O Queue.
            //
            // This counter lives in the priority class, so it will count only queued requests
            // that belong to that class.
            //
            // In other words: the new counter tells you how busy a class is, and the
            // old counter tells you how busy the system is.

            sm::make_queue_length("queue_length", nr_queued, sm::description("Number of requests in the queue"), {io_queue_shard(shard), sm::shard_label(owner), mountlabel, class_label}),
            sm::make_gauge("delay", [this] {
                return queue_time.count();
            }, sm::description("total delay time in the queue"), {io_queue_shard(shard), sm::shard_label(owner), mountlabel, class_label}),
            sm::make_gauge("shares", [this] {
                return this->ptr->shares();
            }, sm::description("current amount of shares"), {io_queue_shard(shard), sm::shard_label(owner), mountlabel, class_label})
    });
    _metric_groups = std::exchange(new_metrics, {});
}

io_queue::priority_class_data& io_queue::find_or_create_class(const io_priority_class& pc, shard_id owner) {
    auto id = pc.id();
    bool do_insert = false;
    if ((do_insert = (owner >= _priority_classes.size()))) {
        _priority_classes.resize(owner + 1);
        _priority_classes[owner].resize(id + 1);
    } else if ((do_insert = (id >= _priority_classes[owner].size()))) {
        _priority_classes[owner].resize(id + 1);
    }
    if (do_insert || !_priority_classes[owner][id]) {
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
        auto pc_data = make_lw_shared<priority_class_data>(name, mountpoint(), pc_ptr, owner);

        _priority_classes[owner][id] = pc_data;
    }
    return *_priority_classes[owner][id];
}

future<io_event>
io_queue::queue_request(const io_priority_class& pc, size_t len, io_queue::request_type req_type, noncopyable_function<void (internal::linux_abi::iocb&)> prepare_io) {
    auto start = std::chrono::steady_clock::now();
    return smp::submit_to(coordinator(), [start, &pc, len, req_type, prepare_io = std::move(prepare_io), owner = engine().cpu_id(), this] () mutable {
        // First time will hit here, and then we create the class. It is important
        // that we create the shared pointer in the same shard it will be used at later.
        auto& pclass = find_or_create_class(pc, owner);
        pclass.nr_queued++;
        unsigned weight;
        size_t size;
        if (req_type == io_queue::request_type::write) {
            weight = _config.disk_req_write_to_read_multiplier;
            size = _config.disk_bytes_write_to_read_multiplier * len;
        } else {
            weight = io_queue::read_request_base_count;
            size = io_queue::read_request_base_count * len;
        }
        auto desc = std::make_unique<io_desc_read_write>(this, weight, size);
        auto fq_desc = desc->fq_descriptor();
        auto fut = desc->get_future();
        _fq.queue(pclass.ptr, std::move(fq_desc), [&pclass, start, prepare_io = std::move(prepare_io), desc = std::move(desc), len, this] () mutable noexcept {
            try {
                pclass.nr_queued--;
                pclass.ops++;
                pclass.bytes += len;
                pclass.queue_time = std::chrono::duration_cast<std::chrono::duration<double>>(std::chrono::steady_clock::now() - start);
                engine().submit_io(desc.get(), std::move(prepare_io));
                desc.release();
            } catch (...) {
                desc->set_exception(std::current_exception());
            }
        });
        return fut;
    });
}

future<>
io_queue::update_shares_for_class(const io_priority_class pc, size_t new_shares) {
    return smp::submit_to(coordinator(), [this, pc, owner = engine().cpu_id(), new_shares] {
        auto& pclass = find_or_create_class(pc, owner);
        _fq.update_shares(pclass.ptr, new_shares);
    });
}

void
io_queue::rename_priority_class(io_priority_class pc, sstring new_name) {
    for (unsigned owner = 0; owner < _priority_classes.size(); owner++) {
        if (_priority_classes[owner].size() > pc.id() &&
                _priority_classes[owner][pc.id()]) {
            _priority_classes[owner][pc.id()]->rename(new_name, _config.mountpoint, owner);
        }
    }
}

}
