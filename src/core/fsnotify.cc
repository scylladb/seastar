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
 * Copyright 2020 ScyllaDB Ltd.
 */

#ifdef SEASTAR_MODULE

module;

#include <cerrno>
#include <cstdint>
#include <stdexcept>
#include <vector>
#include <sys/inotify.h>

module seastar;

#else
#include <seastar/core/internal/pollable_fd.hh>
#include <seastar/core/posix.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/fsnotify.hh>
#endif

namespace seastar::experimental {

class fsnotifier::impl : public enable_shared_from_this<impl> {
    class my_poll_fd : public pollable_fd {
    public:
        using pollable_fd::pollable_fd;
        using pollable_fd::get_fd;

        operator int() const {
            return get_fd();
        }
    };
    my_poll_fd _fd;
    watch_token _close_dummy = -1;
public:
    impl()
        : _fd(file_desc::inotify_init(IN_NONBLOCK | IN_CLOEXEC))
    {}
    void remove_watch(watch_token);
    future<watch_token> create_watch(const sstring& path, flags events);
    future<std::vector<event>> wait();
    void shutdown();
    bool active() const {
        return bool(_fd);
    }
};

void fsnotifier::impl::remove_watch(watch_token token) {
    if (active()) {
        auto res = ::inotify_rm_watch(_fd, token);
        // throw if any other error than EINVAL.
        throw_system_error_on(res == -1 && errno != EINVAL, "could not remove inotify watch");
    }
}

future<fsnotifier::watch_token> fsnotifier::impl::create_watch(const sstring& path, flags events) {
    if (!active()) {
        throw std::runtime_error("attempting to use closed notifier");
    }
    return engine().inotify_add_watch(_fd, path, uint32_t(events));
}

future<std::vector<fsnotifier::event>> fsnotifier::impl::wait() {
    // be paranoid about buffer alignment
    auto buf = temporary_buffer<char>::aligned(std::max(alignof(::inotify_event), alignof(int64_t)), 4096);
    auto f = _fd.read_some(buf.get_write(), buf.size());
    return f.then([me = shared_from_this(), buf = std::move(buf)](size_t n) {
        auto p = buf.get();
        auto e = buf.get() + n;

        std::vector<event> events;

        while (p < e) {
            auto ev = reinterpret_cast<const ::inotify_event*>(p);
            if (ev->wd == me->_close_dummy && me->_close_dummy != -1) {
                me->_fd.close();
            } else {
                events.emplace_back(event {
                    ev->wd, flags(ev->mask), ev->cookie,
                    ev->len != 0 ? sstring(ev->name) : sstring{}
                });
            }
            p += sizeof(::inotify_event) + ev->len;
        }

        return events;
    });
}

void fsnotifier::impl::shutdown() {
    // reactor does not yet have
    // any means of "shutting down" a non-socket read,
    // so we work around this by creating a watch for something ubiquitous,
    // then removing the watch while adding a mark.
    // This will cause any event waiter to wake up, but ignore the event for our
    // dummy.
    (void)create_watch("/", flags::delete_self).then([me = shared_from_this()](watch_token t) {
        me->_close_dummy = t;
        me->remove_watch(t);
    });
}

fsnotifier::watch::~watch() {
    if (_impl) {
        _impl->remove_watch(_token);
    }
}

fsnotifier::watch::watch(watch&&) noexcept = default;
fsnotifier::watch& fsnotifier::watch::operator=(watch&&) noexcept = default;

fsnotifier::watch_token fsnotifier::watch::release() {
    _impl = {};
    return _token;
}

fsnotifier::watch::watch(shared_ptr<impl> impl, watch_token token)
    : _token(token)
    , _impl(std::move(impl))
{}

fsnotifier::fsnotifier()
    : _impl(make_shared<impl>())
{}

fsnotifier::~fsnotifier() = default;

fsnotifier::fsnotifier(fsnotifier&&) = default;
fsnotifier& fsnotifier::operator=(fsnotifier&&) = default;

future<fsnotifier::watch> fsnotifier::create_watch(const sstring& path, flags events) {
    return _impl->create_watch(path, events).then([this](watch_token token) {
        return watch(_impl, token);
    });
}

future<std::vector<fsnotifier::event>> fsnotifier::wait() const {
    return _impl->wait();
}

void fsnotifier::shutdown() {
    _impl->shutdown();
}

bool fsnotifier::active() const {
    return _impl->active();
}

}
