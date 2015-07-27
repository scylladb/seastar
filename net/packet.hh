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

#ifndef PACKET_HH_
#define PACKET_HH_

#include "core/deleter.hh"
#include "core/temporary_buffer.hh"
#include "const.hh"
#include <vector>
#include <cassert>
#include <algorithm>
#include <iosfwd>
#include <experimental/optional>

namespace net {

struct fragment {
    char* base;
    size_t size;
    bool can_merge_with_next;

    fragment(char *b, size_t s, bool m = false) : base(b), size(s), can_merge_with_next(m) {}
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
    std::experimental::optional<uint16_t> vlan_tci;
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
    static constexpr size_t reserved_frags = 2;

    struct pseudo_vector {
        fragment* _start;
        fragment* _finish;
        pseudo_vector(fragment* start, size_t nr)
            : _start(start), _finish(_start + nr) {}
        fragment* begin() { return _start; }
        fragment* end() { return _finish; }
        fragment& operator[](size_t idx) { return _start[idx]; }
    };

// Fragments on packet
//
// To keep trimmed fragments instead of overwriting, we keep trimmed fragment on head and tail.
// We have _frags to point current head of active fragments, and _nr_frags to indicate tail of active fragments.
// Since trim_front() often called, we reserve one fragment on top of _frag_internal, use it trimmed fragment.
//
// Here is pictures of how _frag_internal[] will used:
//
//   initially: _nr_frags = 1, _nr_trimmed_back = 0
//     [0x1000000, 0], [0x1000000, 1512], [uninitialized]
//                     ^_frags
//
//   after trim_front(12): _nr_frags = 1, _nr_trimmed_back = 0
//     [0x1000000, 12], [0x100000c, 1500], [uninitialized]
//                     ^_frags
//   after untrim_front(): _nr_frags = 2, _nr_trimmed_back = 0
//     [0x1000000, 12], [0x100000c, 1500], [uninitialized]
//     ^_frags
//
//   after trim_back(1): _nr_frags = 2, _nr_trimmed_back = 1
//     [0x1000000, 12], [0x100000c, 1499], [0x10005e7, 1]
//     ^_frags
//
    struct impl {
        fragment *_frags = _frags_internal + 1;
        // when destroyed, virtual destructor will reclaim resources
        deleter _deleter;
        unsigned _len = 0;
        uint16_t _nr_frags = 0;
        uint16_t _nr_trimmed_back = 0;
        uint16_t _allocated_frags;
        offload_info _offload_info;
        std::experimental::optional<uint32_t> _rss_hash;
        char _data[internal_data_size]; // only _frags[0] may use
        unsigned _headroom = internal_data_size; // in _data
        // FIXME: share _data/_frags space

        fragment _frags_internal[];

        impl(size_t nr_frags = default_nr_frags);
        impl(const impl&) = delete;
        impl(fragment frag, size_t nr_frags = default_nr_frags);

        pseudo_vector fragments() { return { _frags, _nr_frags }; }
        unsigned nr_trimmed_front() const { return _frags - _frags_internal; }
        bool reserved_front_usable() const { return (nr_trimmed_front() == 1 && _frags_internal[0].size == 0); }
        bool reserved_front_unused() const { return _frags_internal[0].size == 0; }

        static std::unique_ptr<impl> allocate(size_t nr_frags) {
            nr_frags = std::max(nr_frags, default_nr_frags);
            return std::unique_ptr<impl>(new (nr_frags) impl(nr_frags));
        }

        static std::unique_ptr<impl> copy(impl* old, size_t nr) {
            auto n = allocate(old->_allocated_frags - reserved_frags);
            n->_deleter = std::move(old->_deleter);
            n->_len = old->_len;
            n->_nr_frags = old->_nr_frags;
            n->_headroom = old->_headroom;
            n->_offload_info = old->_offload_info;
            n->_rss_hash = old->_rss_hash;
            n->_frags = n->_frags_internal + old->nr_trimmed_front();
            std::copy(old->_frags_internal, old->_frags_internal + old->_allocated_frags, n->_frags_internal);
            old->copy_internal_fragment_to(n.get());

            return std::move(n);
        }

        static std::unique_ptr<impl> copy(impl* old) {
            return copy(old, old->_nr_frags);
        }

        static std::unique_ptr<impl> allocate_if_needed(std::unique_ptr<impl> old, size_t extra_frags) {
            if (old->_allocated_frags >= old->_nr_frags + extra_frags) {
                return std::move(old);
            }
            return copy(old.get(), std::max<size_t>(old->_nr_frags + extra_frags, 2 * old->_nr_frags));
        }
        void* operator new(size_t size, size_t nr_frags = default_nr_frags) {
            assert(nr_frags == uint16_t(nr_frags));
            return ::operator new(size + (nr_frags + 1) * sizeof(fragment));
        }
        // Matching the operator new above
        void operator delete(void* ptr, size_t nr_frags) {
            return ::operator delete(ptr);
        }
        // Since the above "placement delete" hides the global one, expose it
        void operator delete(void* ptr) {
            return ::operator delete(ptr);
        }

        // Check _frags[0] by default
        bool using_internal_data() const {
            return using_internal_data(nr_trimmed_front());
        }

        bool using_internal_data(int i) const {
            return _nr_frags
                    && _frags_internal[i].base >= _data
                    && _frags_internal[i].base < _data + internal_data_size;
        }

        void unuse_internal_data() {
            if (!using_internal_data()) {
                return;
            }
            std::unique_ptr<char[]> buf{new char[_frags[0].size]};
            std::copy(_frags[0].base, _frags[0].base + _frags[0].size,
                    buf.get());
            _frags[0].base = buf.get();
            _deleter = make_deleter(std::move(_deleter), [buf = std::move(buf)] {});
        }
        void copy_internal_fragment_to(impl* to) {
            auto addr = to->_data + _headroom;
            for (auto i = 0; _frags_internal + i < _frags + _nr_frags; i++) {
                if (!using_internal_data(i))
                    break;
                to->_frags_internal[i].base = addr;
                std::copy(_frags_internal[i].base, _frags_internal[i].base + _frags_internal[i].size,
                        to->_frags_internal[i].base);
                addr += _frags_internal[i].size;
            }
        }
    };
    packet(std::unique_ptr<impl>&& impl) : _impl(std::move(impl)) {}
    std::unique_ptr<impl> _impl;
public:
    static packet from_static_data(const char* data, size_t len) {
        return {fragment{const_cast<char*>(data), len}, [] {}};
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
    template <typename Deleter>
    packet(fragment frag, Deleter deleter);
    // zero-copy single fragment
    packet(fragment frag, deleter del);
    // zero-copy multiple fragments
    packet(std::vector<fragment> frag, deleter del);
    // zero-copy multiple fragments
    template <typename Deleter>
    packet(std::vector<fragment> frag, Deleter deleter);
    // build packet with iterator
    template <typename Iterator, typename Deleter>
    packet(Iterator begin, Iterator end, Deleter del);
    // build packet with iterator
    template <typename Iterator>
    packet(Iterator begin, Iterator end, deleter del);
    // append fragment (copying new fragment)
    packet(packet&& x, fragment frag);
    // prepend fragment (copying new fragment, with header optimization)
    packet(fragment frag, packet&& x);
    // prepend fragment (zero-copy)
    template <typename Deleter>
    packet(fragment frag, Deleter deleter, packet&& x);
    // append fragment (zero-copy)
    template <typename Deleter>
    packet(packet&& x, fragment frag, Deleter deleter);
    // append fragment (zero-copy)
    packet(packet&& x, fragment frag, deleter d);
    // append temporary_buffer (zero-copy)
    packet(packet&& x, temporary_buffer<char> buf);
    // append deleter
    template <typename Deleter>
    packet(packet&& x, Deleter d);

    packet& operator=(packet&& x) {
        if (this != &x) {
            this->~packet();
            new (this) packet(std::move(x));
        }
        return *this;
    }

    unsigned len() const { return _impl->_len; }
    unsigned memory() const { return len() +  sizeof(packet::impl); }

    fragment frag(unsigned idx) const { return _impl->_frags[idx]; }
    fragment& frag(unsigned idx) { return _impl->_frags[idx]; }

    unsigned nr_frags() const { return _impl->_nr_frags; }
    unsigned allocated_frags() const { return _impl->_allocated_frags; }
    pseudo_vector fragments() const { return { _impl->_frags, _impl->_nr_frags }; }
    fragment* fragment_array() const { return _impl->_frags; }

    unsigned nr_frags_internal() const { return (_impl->_frags + _impl->_nr_frags) - _impl->_frags_internal; }
    unsigned nr_trimmed_front() const { return _impl->nr_trimmed_front(); }
    unsigned nr_trimmed_back() const { return _impl->_nr_trimmed_back; }
    bool reserved_front_usable() const { return _impl->reserved_front_usable(); }
    bool reserved_front_unused() const { return _impl->reserved_front_unused(); }

    // share packet data (reference counted, non COW)
    packet share();
    packet share(size_t offset, size_t len);

    void append(packet&& p);

    void trim_front(size_t how_much);
    void untrim_front();
    void trim_back(size_t how_much);
    void untrim_back();

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

    void reset() { _impl.reset(); }

    void reserve(int n_frags) {
        if (n_frags > _impl->_nr_frags) {
            auto extra = n_frags - _impl->_nr_frags;
            _impl = impl::allocate_if_needed(std::move(_impl), extra);
        }
    }
    std::experimental::optional<uint32_t> rss_hash() {
        return _impl->_rss_hash;
    }
    std::experimental::optional<uint32_t> set_rss_hash(uint32_t hash) {
        return _impl->_rss_hash = hash;
    }
private:
    void linearize(size_t at_frag, size_t desired_size);
    bool allocate_headroom(size_t size);
public:
    class offload_info offload_info() { return _impl->_offload_info; }
    class offload_info& offload_info_ref() { return _impl->_offload_info; }
    void set_offload_info(class offload_info oi) { _impl->_offload_info = oi; }
};

std::ostream& operator<<(std::ostream& os, const packet& p);

inline
packet::packet(packet&& x) noexcept
    : _impl(std::move(x._impl)) {
}

inline
packet::impl::impl(size_t nr_frags)
    : _len(0), _allocated_frags(nr_frags + reserved_frags) {
    _frags_internal[0] = { nullptr, 0, false };
}

inline
packet::impl::impl(fragment frag, size_t nr_frags)
    : _len(frag.size), _allocated_frags(nr_frags + reserved_frags) {
    assert(_allocated_frags > _nr_frags);
    _frags_internal[0] = { nullptr, 0, false };
    if (frag.size <= internal_data_size) {
        _headroom -= frag.size;
        _frags[0] = { _data + _headroom, frag.size };
    } else {
        std::unique_ptr<char[]> buf{new char[frag.size]};
        _frags[0] = { buf.get(), frag.size };
        _deleter = make_deleter(std::move(_deleter),
                [buf = std::move(buf)] {});
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

template <typename Deleter>
inline
packet::packet(fragment frag, Deleter d) : packet(frag, make_deleter(deleter(), std::move(d))) {}

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

template <typename Deleter>
inline
packet::packet(std::vector<fragment> frag, Deleter d)
    : packet(std::move(frag), make_deleter(deleter(), std::move(d))) {
}

template <typename Iterator, typename Deleter>
inline
packet::packet(Iterator begin, Iterator end, Deleter del) {
    unsigned nr_frags = 0, len = 0;
    nr_frags = std::distance(begin, end);
    std::for_each(begin, end, [&] (fragment& frag) { len += frag.size; });
    _impl = impl::allocate(nr_frags);
    _impl->_deleter = make_deleter(deleter(), std::move(del));
    _impl->_len = len;
    _impl->_nr_frags = nr_frags;
    std::copy(begin, end, _impl->_frags);
}

template <typename Iterator>
inline
packet::packet(Iterator begin, Iterator end, deleter del) {
    unsigned nr_frags = 0, len = 0;
    nr_frags = std::distance(begin, end);
    std::for_each(begin, end, [&] (fragment& frag) { len += frag.size; });
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
            if (reserved_front_usable()) {
                --_impl->_frags;

            } else {
                std::copy_backward(_impl->_frags, _impl->_frags + _impl->_nr_frags,
                        _impl->_frags + _impl->_nr_frags + 1);
            }
            ++_impl->_nr_frags;
            _impl->_frags[0] = { _impl->_data + internal_data_size, 0 };
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
        if (reserved_front_usable()) {
            --_impl->_frags;
        } else {
            std::copy_backward(_impl->_frags, _impl->_frags + _impl->_nr_frags,
                    _impl->_frags + _impl->_nr_frags + 1);
        }
        ++_impl->_nr_frags;
        _impl->_frags[0] = {buf.get(), frag.size};
        _impl->_deleter = make_deleter(std::move(_impl->_deleter),
                [buf = std::move(buf)] {});
    }
}

template <typename Deleter>
inline
packet::packet(fragment frag, Deleter d, packet&& x)
    : _impl(impl::allocate_if_needed(std::move(x._impl), 1)) {
    _impl->unuse_internal_data();
    _impl->_len += frag.size;
    if (reserved_front_usable()) {
        --_impl->_frags;
    } else {
        std::copy_backward(_impl->_frags, _impl->_frags + _impl->_nr_frags,
                _impl->_frags + _impl->_nr_frags + 1);
    }
    ++_impl->_nr_frags;
    _impl->_frags[0] = frag;

    _impl->_deleter = make_deleter(std::move(_impl->_deleter), std::move(d));
}

template <typename Deleter>
inline
packet::packet(packet&& x, fragment frag, Deleter d)
    : _impl(impl::allocate_if_needed(std::move(x._impl), 1)) {
    _impl->_len += frag.size;
    _impl->_frags[_impl->_nr_frags++] = frag;
    _impl->_deleter = make_deleter(std::move(_impl->_deleter), std::move(d));
}

inline
packet::packet(packet&& x, fragment frag, deleter d)
    : _impl(impl::allocate_if_needed(std::move(x._impl), 1)) {
    _impl->_len += frag.size;
    _impl->_frags[_impl->_nr_frags++] = frag;
    d.append(std::move(_impl->_deleter));
    _impl->_deleter = std::move(d);
}

template <typename Deleter>
inline
packet::packet(packet&& x, Deleter d)
    : _impl(std::move(x._impl)) {
    _impl->_deleter = make_deleter(std::move(_impl->_deleter), std::move(d));
}

inline
packet::packet(packet&& x, temporary_buffer<char> buf)
    : packet(std::move(x), fragment{buf.get_write(), buf.size()}, buf.release()) {
}

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
void packet::trim_front(size_t how_much) {
    assert(how_much <= _impl->_len);
    _impl->_len -= how_much;
    size_t i = 0;
    while (how_much && how_much >= _impl->_frags[i].size) {
        how_much -= _impl->_frags[i].size;
        // if _frags[i-1] and _frags[i] is contiguous, merge them
        if (nr_trimmed_front() && _impl->_frags[i - 1].can_merge_with_next &&
            _impl->_frags[i - 1].size) {
            _impl->_frags[i - 1].size += _impl->_frags[i].size;
            _impl->_frags[i - 1].can_merge_with_next = false;
            if (_impl->_nr_frags > i) {
                std::copy(_impl->_frags + i + 1, _impl->_frags + nr_frags() + nr_trimmed_back(), _impl->_frags + i);
            }
            --_impl->_nr_frags;
        } else {
            ++i;
        }
    }
    // move _frags pointer to _frags[i]
    _impl->_frags += i;
    _impl->_nr_frags -= i;
    if (!_impl->using_internal_data()) {
        _impl->_headroom = internal_data_size;
    }
    if (how_much) {
        if (_impl->using_internal_data()) {
            _impl->_headroom += how_much;
        }
        // we can move bytes from _frags[0] to _frags[-1] since it's contiguous
        if (nr_trimmed_front() && _impl->_frags[-1].can_merge_with_next) {
            _impl->_frags[-1].size += how_much;
            _impl->_frags[0].base += how_much;
            _impl->_frags[0].size -= how_much;
        // if reserved front available, move how_much bytes from _frags[0] to _frags[-1]
        } else if (reserved_front_usable()) {
            _impl->_frags[-1] = { _impl->_frags[0].base, how_much, true };
            _impl->_frags[0].base += how_much;
            _impl->_frags[0].size -= how_much;
        // move reserved front to _frags[-1]
        } else if (reserved_front_unused()) {
            std::copy(_impl->_frags_internal + 1, _impl->_frags, _impl->_frags_internal);
            _impl->_frags[-1] = { _impl->_frags[0].base, how_much, true };
            _impl->_frags[0].base += how_much;
            _impl->_frags[0].size -= how_much;
        // insert one fragment on _frags[0],  move how_much bytes from _frags[1] to _frags[0] then trim it
        } else {
            std::copy(_impl->_frags, _impl->_frags + nr_frags() + nr_trimmed_back(), _impl->_frags + 1);
            _impl->_frags[0] = { _impl->_frags[1].base, how_much, true };
            _impl->_frags[1].base += how_much;
            _impl->_frags[1].size -= how_much;
            ++_impl->_frags;
        }
    }
}

inline
void packet::untrim_front() {
    for (auto f = _impl->_frags_internal; f < _impl->_frags; f++) {
        _impl->_len += f->size;
        ++_impl->_nr_frags;
    }
    _impl->_frags = _impl->_frags_internal;
    if (_impl->_frags[0].size == 0) {
        ++_impl->_frags;
        --_impl->_nr_frags;
    }
}

inline
void packet::trim_back(size_t how_much) {
    assert(how_much <= _impl->_len);
    _impl->_len -= how_much;
    size_t i = _impl->_nr_frags - 1;
    while (how_much && how_much >= _impl->_frags[i].size) {
        how_much -= _impl->_frags[i].size;
        // if _frags[i] and _frags[i+1] is contiguous, merge them
        if (nr_trimmed_back() + _impl->_frags[i].can_merge_with_next) {
            _impl->_frags[i].size += _impl->_frags[i + 1].size;
            _impl->_frags[i].can_merge_with_next = false;
            if (nr_trimmed_back() > 1) {
                std::copy(_impl->_frags + i + 2, _impl->_frags + nr_frags() + nr_trimmed_back(), _impl->_frags + i + 1);
            }
        } else {
            ++_impl->_nr_trimmed_back;
        }
        --_impl->_nr_frags;
        --i;
    }
    if (how_much) {
        _impl->_frags[i].size -= how_much;
        if (i == 0 && _impl->using_internal_data()) {
            _impl->_headroom += how_much;
        }
        // we can move bytes from _frags[i] to _frags[i+1] since it's contiguous
        if (_impl->_nr_frags > i &&
            _impl->_frags[i].can_merge_with_next) {
            _impl->_frags[i + 1].base -= how_much;
            _impl->_frags[i + 1].size += how_much;
        // insert one fragment on _frags[i+1],  move how_much bytes from _frags[i] to _frags[i+1] then trim it
        } else {
            std::copy(_impl->_frags + i + 1, _impl->_frags + nr_frags() + nr_trimmed_back(), _impl->_frags + i + 2);
            _impl->_frags[i + 1] = { _impl->_frags[i].base + _impl->_frags[i].size, how_much, false };
            _impl->_frags[i].can_merge_with_next = true;
            ++_impl->_nr_trimmed_back;
        }
    }
}

inline
void packet::untrim_back() {
    for (auto f = _impl->_frags + _impl->_nr_frags; _impl->_nr_trimmed_back > 0; f++) {
        _impl->_len += f->size;
        ++_impl->_nr_frags;
        --_impl->_nr_trimmed_back;
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
        _impl->_len += size;
        _impl = impl::allocate_if_needed(std::move(_impl), 1);
        std::unique_ptr<char[]> buf(new char[size]);
        if (reserved_front_usable()) {
            --_impl->_frags;
        } else {
            std::copy_backward(_impl->_frags, _impl->_frags + _impl->_nr_frags,
                    _impl->_frags + _impl->_nr_frags + 1);
        }
        ++_impl->_nr_frags;
        _impl->_frags[0] = {buf.get(), size};
        _impl->_deleter = make_deleter(std::move(_impl->_deleter),
                [buf = std::move(buf)] {});
    }
    return _impl->_frags[0].base;
}

inline
packet packet::share() {
    return share(0, _impl->_len);
}

inline
packet packet::share(size_t offset, size_t l) {
    _impl->unuse_internal_data(); // FIXME: eliminate?
    packet n;
    n._impl = impl::allocate_if_needed(std::move(n._impl), _impl->_nr_frags);
    n._impl->_frags += nr_trimmed_front();
    n._impl->_nr_frags = nr_frags();
    n._impl->_nr_trimmed_back = nr_trimmed_back();
    n._impl->_len = len();
    std::copy(_impl->_frags_internal, _impl->_frags_internal + _impl->_allocated_frags, n._impl->_frags_internal);
    if (offset > 0)
        n.trim_front(offset);
    if (l < n.len())
        n.trim_back(l);

    n._impl->_offload_info = _impl->_offload_info;
    assert(!n._impl->_deleter);
    n._impl->_deleter = _impl->_deleter.share();
    return n;
}

}

#endif /* PACKET_HH_ */
