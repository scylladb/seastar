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
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 */

#pragma once

#include <seastar/core/deleter.hh>
#include <seastar/core/temporary_buffer.hh>
#include <seastar/net/const.hh>
#include <seastar/util/assert.hh>
#include <seastar/util/std-compat.hh>
#include <seastar/util/modules.hh>
#ifndef SEASTAR_MODULE
#include <algorithm>
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <memory>
#include <optional>
#include <vector>
#endif

namespace seastar {

namespace net {

SEASTAR_MODULE_EXPORT_BEGIN

struct fragment {
    char* base;
    size_t size;
};

struct offload_info {
    ip_protocol_num protocol = ip_protocol_num::unused;
    bool needs_csum = false;
    uint8_t ip_hdr_len = 20;
    uint8_t tcp_hdr_len = 20;
    uint8_t udp_hdr_len = 8;
    bool needs_ip_csum = false;
    bool reassembled = false;
    uint16_t tso_seg_size = 0;
    // HW stripped VLAN header (CPU order)
    std::optional<uint16_t> vlan_tci;
};

// Zero-copy friendly packet class
//
// For implementing zero-copy, we need a flexible destructor that can
// destroy packet data in different ways: decrementing a reference count,
// or calling a free()-like function.
//
// Moreover, we need different destructors for each set of fragments within
// a single fragment. For example, a header and trailer might need delete[]
// to be called, while the internal data needs a reference count to be
// released.  Matters are complicated in that fragments can be split
// (due to virtual/physical translation).
//
// To implement this, we associate each packet with a single destructor,
// but allow composing a packet from another packet plus a fragment to
// be added, with its own destructor, causing the destructors to be chained.
//
// The downside is that the data needed for the destructor is duplicated,
// if it is already available in the fragment itself.
//
// As an optimization, when we allocate small fragments, we allocate some
// extra space, so prepending to the packet does not require extra
// allocations.  This is useful when adding headers.
//
class packet final {
    // enough for lots of headers, not quite two cache lines:
    static constexpr size_t internal_data_size = 128 - 16;
    static constexpr size_t default_nr_frags = 4;

    struct pseudo_vector {
        fragment* _start;
        fragment* _finish;
        pseudo_vector(fragment* start, size_t nr) noexcept
            : _start(start), _finish(_start + nr) {}
        fragment* begin() noexcept { return _start; }
        fragment* end() noexcept { return _finish; }
        fragment& operator[](size_t idx) noexcept { return _start[idx]; }
    };

    struct impl {
        // when destroyed, virtual destructor will reclaim resources
        deleter _deleter;
        unsigned _len = 0;
        uint16_t _nr_frags = 0;
        uint16_t _allocated_frags;
        offload_info _offload_info;
        std::optional<uint32_t> _rss_hash;
        char _data[internal_data_size]; // only _frags[0] may use
        unsigned _headroom = internal_data_size; // in _data
        // FIXME: share _data/_frags space

        fragment _frags[];

        impl(size_t nr_frags = default_nr_frags) noexcept;
        impl(const impl&) = delete;
        impl(fragment frag, size_t nr_frags = default_nr_frags);

        pseudo_vector fragments() noexcept { return { _frags, _nr_frags }; }

        static std::unique_ptr<impl> allocate(size_t nr_frags) {
            nr_frags = std::max(nr_frags, default_nr_frags);
            return std::unique_ptr<impl>(new (nr_frags) impl(nr_frags));
        }

        static std::unique_ptr<impl> copy(impl* old, size_t nr) {
            auto n = allocate(nr);
            n->_deleter = std::move(old->_deleter);
            n->_len = old->_len;
            n->_nr_frags = old->_nr_frags;
            n->_headroom = old->_headroom;
            n->_offload_info = old->_offload_info;
            n->_rss_hash = old->_rss_hash;
            std::copy(old->_frags, old->_frags + old->_nr_frags, n->_frags);
            old->copy_internal_fragment_to(n.get());
            return n;
        }

        static std::unique_ptr<impl> copy(impl* old) {
            return copy(old, old->_nr_frags);
        }

        static std::unique_ptr<impl> allocate_if_needed(std::unique_ptr<impl> old, size_t extra_frags) {
            if (old->_allocated_frags >= old->_nr_frags + extra_frags) {
                return old;
            }
            return copy(old.get(), std::max<size_t>(old->_nr_frags + extra_frags, 2 * old->_nr_frags));
        }
        void* operator new(size_t size, size_t nr_frags = default_nr_frags) {
            SEASTAR_ASSERT(nr_frags == uint16_t(nr_frags));
            return ::operator new(size + nr_frags * sizeof(fragment));
        }
        // Matching the operator new above
        void operator delete(void* ptr, size_t) {
            return ::operator delete(ptr);
        }
        // Since the above "placement delete" hides the global one, expose it
        void operator delete(void* ptr) {
            return ::operator delete(ptr);
        }

        bool using_internal_data() const noexcept {
            return _nr_frags
                    && _frags[0].base >= _data
                    && _frags[0].base < _data + internal_data_size;
        }

        void unuse_internal_data() {
            if (!using_internal_data()) {
                return;
            }
            auto buf = static_cast<char*>(::malloc(_frags[0].size));
            if (!buf) {
                throw std::bad_alloc();
            }
            deleter d = make_free_deleter(buf);
            std::copy(_frags[0].base, _frags[0].base + _frags[0].size, buf);
            _frags[0].base = buf;
            d.append(std::move(_deleter));
            _deleter = std::move(d);
            _headroom = internal_data_size;
        }
        void copy_internal_fragment_to(impl* to) noexcept {
            if (!using_internal_data()) {
                return;
            }
            to->_frags[0].base = to->_data + _headroom;
            std::copy(_frags[0].base, _frags[0].base + _frags[0].size,
                    to->_frags[0].base);
        }
    };
    packet(std::unique_ptr<impl>&& impl) noexcept : _impl(std::move(impl)) {}
    std::unique_ptr<impl> _impl;
public:
    static packet from_static_data(const char* data, size_t len) noexcept {
        return {fragment{const_cast<char*>(data), len}, deleter()};
    }

    // build empty packet
    packet();
    // build empty packet with nr_frags allocated
    packet(size_t nr_frags);
    // move existing packet
    packet(packet&& x) noexcept;
    // copy data into packet
    packet(const char* data, size_t len);
    // copy data into packet
    packet(fragment frag);
    // zero-copy single fragment
    packet(fragment frag, deleter del);
    // zero-copy multiple fragments
    packet(std::vector<fragment> frag, deleter del);
    // build packet with iterator
    template <typename Iterator>
    packet(Iterator begin, Iterator end, deleter del);
    // append fragment (copying new fragment)
    packet(packet&& x, fragment frag);
    // prepend fragment (copying new fragment, with header optimization)
    packet(fragment frag, packet&& x);
    // prepend fragment (zero-copy)
    packet(fragment frag, deleter del, packet&& x);
    // append fragment (zero-copy)
    packet(packet&& x, fragment frag, deleter d);
    // append temporary_buffer (zero-copy)
    packet(packet&& x, temporary_buffer<char> buf);
    // create from temporary_buffer (zero-copy)
    packet(temporary_buffer<char> buf);
    // append deleter
    packet(packet&& x, deleter d);

    packet& operator=(packet&& x) noexcept {
        if (this != &x) {
            this->~packet();
            new (this) packet(std::move(x));
        }
        return *this;
    }

    unsigned len() const noexcept { return _impl->_len; }
    unsigned memory() const noexcept { return len() +  sizeof(packet::impl); }

    fragment frag(unsigned idx) const noexcept { return _impl->_frags[idx]; }
    fragment& frag(unsigned idx) noexcept { return _impl->_frags[idx]; }

    unsigned nr_frags() const noexcept { return _impl->_nr_frags; }
    pseudo_vector fragments() const noexcept { return { _impl->_frags, _impl->_nr_frags }; }
    fragment* fragment_array() const noexcept { return _impl->_frags; }

    // share packet data (reference counted, non COW)
    packet share();
    packet share(size_t offset, size_t len);

    void append(packet&& p);

    void trim_front(size_t how_much) noexcept;
    void trim_back(size_t how_much) noexcept;

    // get a header pointer, linearizing if necessary
    template <typename Header>
    Header* get_header(size_t offset = 0);

    // get a header pointer, linearizing if necessary
    char* get_header(size_t offset, size_t size);

    // prepend a header (default-initializing it)
    template <typename Header>
    Header* prepend_header(size_t extra_size = 0);

    // prepend a header (uninitialized!)
    char* prepend_uninitialized_header(size_t size);

    packet free_on_cpu(unsigned cpu, std::function<void()> cb = []{});

    void linearize() { return linearize(0, len()); }

    void reset() noexcept { _impl.reset(); }

    void reserve(int n_frags) {
        if (n_frags > _impl->_nr_frags) {
            auto extra = n_frags - _impl->_nr_frags;
            _impl = impl::allocate_if_needed(std::move(_impl), extra);
        }
    }
    std::optional<uint32_t> rss_hash() const noexcept {
        return _impl->_rss_hash;
    }
    std::optional<uint32_t> set_rss_hash(uint32_t hash) noexcept {
        return _impl->_rss_hash = hash;
    }
    // Call `func` for each fragment, avoiding data copies when possible
    // `func` is called with a temporary_buffer<char> parameter
    template <typename Func>
    void release_into(Func&& func) {
        unsigned idx = 0;
        if (_impl->using_internal_data()) {
            auto&& f = frag(idx++);
            func(temporary_buffer<char>(f.base, f.size));
        }
        while (idx < nr_frags()) {
            auto&& f = frag(idx++);
            func(temporary_buffer<char>(f.base, f.size, _impl->_deleter.share()));
        }
    }
    std::vector<temporary_buffer<char>> release() {
        std::vector<temporary_buffer<char>> ret;
        ret.reserve(_impl->_nr_frags);
        release_into([&ret] (temporary_buffer<char>&& frag) {
            ret.push_back(std::move(frag));
        });
        return ret;
    }
    explicit operator bool() noexcept {
        return bool(_impl);
    }
    static packet make_null_packet() noexcept {
        return net::packet(nullptr);
    }
private:
    void linearize(size_t at_frag, size_t desired_size);
    bool allocate_headroom(size_t size);
public:
    struct offload_info get_offload_info() const noexcept { return _impl->_offload_info; }
    struct offload_info& offload_info_ref() noexcept { return _impl->_offload_info; }
    void set_offload_info(struct offload_info oi) noexcept { _impl->_offload_info = oi; }
};

std::ostream& operator<<(std::ostream& os, const packet& p);

SEASTAR_MODULE_EXPORT_END

inline
packet::packet(packet&& x) noexcept
    : _impl(std::move(x._impl)) {
}

inline
packet::impl::impl(size_t nr_frags) noexcept
    : _len(0), _allocated_frags(nr_frags) {
}

inline
packet::impl::impl(fragment frag, size_t nr_frags)
    : _len(frag.size), _allocated_frags(nr_frags) {
    SEASTAR_ASSERT(_allocated_frags > _nr_frags);
    if (frag.size <= internal_data_size) {
        _headroom -= frag.size;
        _frags[0] = { _data + _headroom, frag.size };
    } else {
        auto buf = static_cast<char*>(::malloc(frag.size));
        if (!buf) {
            throw std::bad_alloc();
        }
        deleter d = make_free_deleter(buf);
        _frags[0] = { buf, frag.size };
        _deleter.append(std::move(d));
    }
    std::copy(frag.base, frag.base + frag.size, _frags[0].base);
    ++_nr_frags;
}

inline
packet::packet()
    : _impl(impl::allocate(1)) {
}

inline
packet::packet(size_t nr_frags)
    : _impl(impl::allocate(nr_frags)) {
}

inline
packet::packet(fragment frag) : _impl(new impl(frag)) {
}

inline
packet::packet(const char* data, size_t size) : packet(fragment{const_cast<char*>(data), size}) {
}

inline
packet::packet(fragment frag, deleter d)
    : _impl(impl::allocate(1)) {
    _impl->_deleter = std::move(d);
    _impl->_frags[_impl->_nr_frags++] = frag;
    _impl->_len = frag.size;
}

inline
packet::packet(std::vector<fragment> frag, deleter d)
    : _impl(impl::allocate(frag.size())) {
    _impl->_deleter = std::move(d);
    std::copy(frag.begin(), frag.end(), _impl->_frags);
    _impl->_nr_frags = frag.size();
    _impl->_len = 0;
    for (auto&& f : _impl->fragments()) {
        _impl->_len += f.size;
    }
}

template <typename Iterator>
inline
packet::packet(Iterator begin, Iterator end, deleter del) {
    unsigned nr_frags = 0, len = 0;
    nr_frags = std::distance(begin, end);
    std::for_each(begin, end, [&] (const fragment& frag) { len += frag.size; });
    _impl = impl::allocate(nr_frags);
    _impl->_deleter = std::move(del);
    _impl->_len = len;
    _impl->_nr_frags = nr_frags;
    std::copy(begin, end, _impl->_frags);
}

inline
packet::packet(packet&& x, fragment frag)
    : _impl(impl::allocate_if_needed(std::move(x._impl), 1)) {
    _impl->_len += frag.size;
    std::unique_ptr<char[]> buf(new char[frag.size]);
    std::copy(frag.base, frag.base + frag.size, buf.get());
    _impl->_frags[_impl->_nr_frags++] = {buf.get(), frag.size};
    _impl->_deleter = make_deleter(std::move(_impl->_deleter), [buf = buf.release()] {
        delete[] buf;
    });
}

inline
bool
packet::allocate_headroom(size_t size) {
    if (_impl->_headroom >= size) {
        _impl->_len += size;
        if (!_impl->using_internal_data()) {
            _impl = impl::allocate_if_needed(std::move(_impl), 1);
            std::copy_backward(_impl->_frags, _impl->_frags + _impl->_nr_frags,
                    _impl->_frags + _impl->_nr_frags + 1);
            _impl->_frags[0] = { _impl->_data + internal_data_size, 0 };
            ++_impl->_nr_frags;
        }
        _impl->_headroom -= size;
        _impl->_frags[0].base -= size;
        _impl->_frags[0].size += size;
        return true;
    } else {
        return false;
    }
}


inline
packet::packet(fragment frag, packet&& x)
    : _impl(std::move(x._impl)) {
    // try to prepend into existing internal fragment
    if (allocate_headroom(frag.size)) {
        std::copy(frag.base, frag.base + frag.size, _impl->_frags[0].base);
        return;
    } else {
        // didn't work out, allocate and copy
        _impl->unuse_internal_data();
        _impl = impl::allocate_if_needed(std::move(_impl), 1);
        _impl->_len += frag.size;
        std::unique_ptr<char[]> buf(new char[frag.size]);
        std::copy(frag.base, frag.base + frag.size, buf.get());
        std::copy_backward(_impl->_frags, _impl->_frags + _impl->_nr_frags,
                _impl->_frags + _impl->_nr_frags + 1);
        ++_impl->_nr_frags;
        _impl->_frags[0] = {buf.get(), frag.size};
        _impl->_deleter = make_deleter(std::move(_impl->_deleter),
                [buf = std::move(buf)] {});
    }
}

inline
packet::packet(packet&& x, fragment frag, deleter d)
    : _impl(impl::allocate_if_needed(std::move(x._impl), 1)) {
    _impl->_len += frag.size;
    _impl->_frags[_impl->_nr_frags++] = frag;
    d.append(std::move(_impl->_deleter));
    _impl->_deleter = std::move(d);
}

inline
packet::packet(packet&& x, deleter d)
    : _impl(std::move(x._impl)) {
    _impl->_deleter.append(std::move(d));
}

inline
packet::packet(packet&& x, temporary_buffer<char> buf)
    : packet(std::move(x), fragment{buf.get_write(), buf.size()}, buf.release()) {
}

inline
packet::packet(temporary_buffer<char> buf)
    : packet(fragment{buf.get_write(), buf.size()}, buf.release()) {}

inline
void packet::append(packet&& p) {
    if (!_impl->_len) {
        *this = std::move(p);
        return;
    }
    _impl = impl::allocate_if_needed(std::move(_impl), p._impl->_nr_frags);
    _impl->_len += p._impl->_len;
    p._impl->unuse_internal_data();
    std::copy(p._impl->_frags, p._impl->_frags + p._impl->_nr_frags,
            _impl->_frags + _impl->_nr_frags);
    _impl->_nr_frags += p._impl->_nr_frags;
    p._impl->_deleter.append(std::move(_impl->_deleter));
    _impl->_deleter = std::move(p._impl->_deleter);
}

inline
char* packet::get_header(size_t offset, size_t size) {
    if (offset + size > _impl->_len) {
        return nullptr;
    }
    size_t i = 0;
    while (i != _impl->_nr_frags && offset >= _impl->_frags[i].size) {
        offset -= _impl->_frags[i++].size;
    }
    if (i == _impl->_nr_frags) {
        return nullptr;
    }
    if (offset + size > _impl->_frags[i].size) {
        linearize(i, offset + size);
    }
    return _impl->_frags[i].base + offset;
}

template <typename Header>
inline
Header* packet::get_header(size_t offset) {
    return reinterpret_cast<Header*>(get_header(offset, sizeof(Header)));
}

inline
void packet::trim_front(size_t how_much) noexcept {
    SEASTAR_ASSERT(how_much <= _impl->_len);
    _impl->_len -= how_much;
    size_t i = 0;
    while (how_much && how_much >= _impl->_frags[i].size) {
        how_much -= _impl->_frags[i++].size;
    }
    std::copy(_impl->_frags + i, _impl->_frags + _impl->_nr_frags, _impl->_frags);
    _impl->_nr_frags -= i;
    if (!_impl->using_internal_data()) {
        _impl->_headroom = internal_data_size;
    }
    if (how_much) {
        if (_impl->using_internal_data()) {
            _impl->_headroom += how_much;
        }
        _impl->_frags[0].base += how_much;
        _impl->_frags[0].size -= how_much;
    }
}

inline
void packet::trim_back(size_t how_much) noexcept {
    SEASTAR_ASSERT(how_much <= _impl->_len);
    _impl->_len -= how_much;
    size_t i = _impl->_nr_frags - 1;
    while (how_much && how_much >= _impl->_frags[i].size) {
        how_much -= _impl->_frags[i--].size;
    }
    _impl->_nr_frags = i + 1;
    if (how_much) {
        _impl->_frags[i].size -= how_much;
        if (i == 0 && _impl->using_internal_data()) {
            _impl->_headroom += how_much;
        }
    }
}

template <typename Header>
Header*
packet::prepend_header(size_t extra_size) {
    auto h = prepend_uninitialized_header(sizeof(Header) + extra_size);
    return new (h) Header{};
}

// prepend a header (uninitialized!)
inline
char* packet::prepend_uninitialized_header(size_t size) {
    if (!allocate_headroom(size)) {
        // didn't work out, allocate and copy
        _impl->unuse_internal_data();
        // try again, after unuse_internal_data we may have space after all
        if (!allocate_headroom(size)) {
            // failed
            _impl->_len += size;
            _impl = impl::allocate_if_needed(std::move(_impl), 1);
            std::unique_ptr<char[]> buf(new char[size]);
            std::copy_backward(_impl->_frags, _impl->_frags + _impl->_nr_frags,
                    _impl->_frags + _impl->_nr_frags + 1);
            ++_impl->_nr_frags;
            _impl->_frags[0] = {buf.get(), size};
            _impl->_deleter = make_deleter(std::move(_impl->_deleter),
                    [buf = std::move(buf)] {});
        }
    }
    return _impl->_frags[0].base;
}

inline
packet packet::share() {
    return share(0, _impl->_len);
}

inline
packet packet::share(size_t offset, size_t len) {
    _impl->unuse_internal_data(); // FIXME: eliminate?
    packet n;
    n._impl = impl::allocate_if_needed(std::move(n._impl), _impl->_nr_frags);
    size_t idx = 0;
    while (offset > 0 && offset >= _impl->_frags[idx].size) {
        offset -= _impl->_frags[idx++].size;
    }
    while (n._impl->_len < len) {
        auto& f = _impl->_frags[idx++];
        auto fsize = std::min(len - n._impl->_len, f.size - offset);
        n._impl->_frags[n._impl->_nr_frags++] = { f.base + offset, fsize };
        n._impl->_len += fsize;
        offset = 0;
    }
    n._impl->_offload_info = _impl->_offload_info;
    SEASTAR_ASSERT(!n._impl->_deleter);
    n._impl->_deleter = _impl->_deleter.share();
    return n;
}

}

}
