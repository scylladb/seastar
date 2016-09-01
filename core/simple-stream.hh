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
 * Copyright (C) 2016 Scylladb, Ltd.
 */

#pragma once
#include "core/sstring.hh"

namespace seastar {

class measuring_output_stream {
    size_t _size = 0;
public:
    void write(const char* data, size_t size) {
        _size += size;
    }

    size_t size() const {
        return _size;
    }
};

class simple_output_stream {
    char* _p;
public:
    simple_output_stream(sstring& s, size_t start = 0) : _p(s.begin() + start) {}
    simple_output_stream(char* s, size_t start = 0) : _p(reinterpret_cast<char*>(s) + start) {}
    void write(const char* data, size_t size) {
        _p = std::copy_n(data, size, _p);
    }
};

struct simple_stream_tag {};

/*
template<typename Visitor>
concept bool StreamVisitor() {
    return requires(Visitor visitor, simple& simple, fragmented& fragmented) {
        visitor(simple);
        visitor(fragmented);
    };
}
*/
// memory_stream performs type erasure optimized for cases where
// simple is used.
// By using a lot of [[gnu::always_inline]] attributes this class attempts to
// make the compiler generate code with simple functions inlined
// directly in the user of the intput_stream.
template<typename Iterator>
class memory_stream {
public:
    class simple {
        const char* _p = nullptr;
        size_t _size = 0;
    public:
        using has_with_stream = std::false_type;
        using iterator_type = Iterator;
        simple() {}
        simple(const char* p, size_t size) : _p(p), _size(size) {}
        const char* begin() const { return _p; }

        [[gnu::always_inline]]
        void skip(size_t size) {
            if (size > _size) {
                throw std::out_of_range("deserialization buffer underflow");
            }
            _p += size;
            _size -= size;
        }

        [[gnu::always_inline]]
        simple read_substream(size_t size) {
           if (size > _size) {
               throw std::out_of_range("deserialization buffer underflow");
           }
           simple substream(_p, size);
           skip(size);
           return substream;
        }

        [[gnu::always_inline]]
        void read(char* p, size_t size) {
            if (size > _size) {
                throw std::out_of_range("deserialization buffer underflow");
            }
            std::copy_n(_p, size, p);
            skip(size);
        }

        template<typename Output>
        [[gnu::always_inline]]
        void copy_to(Output& out) const {
            out.write(_p, _size);
        }

        [[gnu::always_inline]]
        const size_t size() const {
            return _size;
        }
    };

    class fragmented {
        Iterator _it;
        simple _current;
        size_t _size;
    private:
        template<typename Func>
        //requires requires(Func f, view bv) { { f(bv) } -> void; }
        void for_each_fragment(size_t size, Func&& func) {
            if (size > _size) {
                throw std::out_of_range("deserialization buffer underflow");
            }
            _size -= size;
            while (size) {
                if (!_current.size()) {
                    _current = simple(reinterpret_cast<const char*>((*_it).begin()), (*_it).size());
                    _it++;
                }
                auto this_size = std::min(_current.size(), size);
                func(_current.read_substream(this_size));
                size -= this_size;
            }
        }
        fragmented(Iterator it, simple bv, size_t size)
            : _it(it), _current(bv), _size(size) { }
    public:
        using has_with_stream = std::false_type;
        using iterator_type = Iterator;
        fragmented(Iterator it, size_t size)
            : _it(it), _size(size) {
        }

        void skip(size_t size) {
            for_each_fragment(size, [] (auto) { });
        }
        fragmented read_substream(size_t size) {
            if (size > _size) {
                throw std::out_of_range("deserialization buffer underflow");
            }
            fragmented substream(_it, _current, size);
            skip(size);
            return substream;
        }
        void read(char* p, size_t size) {
            for_each_fragment(size, [&p] (auto bv) {
                p = std::copy_n(bv.begin(), bv.size(), p);
            });
        }
        template<typename Output>
        void copy_to(Output& out) {
            for_each_fragment(_size, [&out] (auto bv) {
                bv.copy_to(out);
            });
        }
        const size_t size() const {
            return _size;
        }
    };
private:
    const bool _is_simple;
    using fragmented_type = std::conditional_t<std::is_same<Iterator, simple_stream_tag>::value, simple, fragmented>;
    union {
        simple _simple;
        fragmented_type _fragmented;
    };
public:
    template<typename StreamVisitor>
    [[gnu::always_inline]]
    decltype(auto) with_stream(StreamVisitor&& visitor) {
        if (__builtin_expect(_is_simple, true)) {
            return visitor(_simple);
        }
        return visitor(_fragmented);
    }

    template<typename StreamVisitor>
    [[gnu::always_inline]]
    decltype(auto) with_stream(StreamVisitor&& visitor) const {
        if (__builtin_expect(_is_simple, true)) {
            return visitor(_simple);
        }
        return visitor(_fragmented);
    }
public:
    using has_with_stream = std::true_type;
    using iterator_type = Iterator;
    memory_stream(simple stream)
            : _is_simple(true), _simple(std::move(stream)) {}
    memory_stream(fragmented stream)
            : _is_simple(false), _fragmented(std::move(stream)) {}

    [[gnu::always_inline]]
    memory_stream(const memory_stream& other) noexcept : _is_simple(other._is_simple) {
        // Making this copy constructor noexcept makes copy assignment simpler.
        // Besides, performance of memory_stream relies on the fact that both
        // fragmented and simple input stream are PODs and the branch below
        // is optimized away, so throwable copy constructors aren't something
        // we want.
        static_assert(std::is_nothrow_copy_constructible<fragmented>::value,
                      "seastar::memory_stream::fragmented should be copy constructible");
        static_assert(std::is_nothrow_copy_constructible<simple>::value,
                      "seastar::memory_stream::simple should be copy constructible");
        if (_is_simple) {
            new (&_simple) simple(other._simple);
        } else {
            new (&_fragmented) fragmented_type(other._fragmented);
        }
    }

    [[gnu::always_inline]]
    memory_stream(memory_stream&& other) noexcept : _is_simple(other._is_simple) {
        if (_is_simple) {
            new (&_simple) simple(std::move(other._simple));
        } else {
            new (&_fragmented) fragmented_type(std::move(other._fragmented));
        }
    }

    [[gnu::always_inline]]
    memory_stream& operator=(const memory_stream& other) noexcept {
        // Copy constructor being noexcept makes copy assignment simpler.
        static_assert(std::is_nothrow_copy_constructible<memory_stream>::value,
                      "memory_stream copy constructor shouldn't throw");
        if (this != &other) {
            this->~memory_stream();
            new (this) memory_stream(other);
        }
        return *this;
    }

    [[gnu::always_inline]]
    memory_stream& operator=(memory_stream&& other) noexcept {
        if (this != &other) {
            this->~memory_stream();
            new (this) memory_stream(std::move(other));
        }
        return *this;
    }

    [[gnu::always_inline]]
    ~memory_stream() {
        if (_is_simple) {
            _simple.~simple();
        } else {
            _fragmented.~fragmented_type();
        }
    }

    [[gnu::always_inline]]
    void skip(size_t size) {
        with_stream([size] (auto& stream) {
            stream.skip(size);
        });
    }

    [[gnu::always_inline]]
    memory_stream read_substream(size_t size) {
        return with_stream([size] (auto& stream) -> memory_stream {
            return stream.read_substream(size);
        });
    }

    [[gnu::always_inline]]
    void read(char* p, size_t size) {
        with_stream([p, size] (auto& stream) {
            stream.read(p, size);
        });
    }

    template<typename Output>
    [[gnu::always_inline]]
    void copy_to(Output& out) {
        with_stream([&out] (auto& stream) {
            stream.copy_to(out);
        });
    }

    [[gnu::always_inline]]
    size_t size() const {
        return with_stream([] (auto& stream) {
            return stream.size();
        });
    }

    template<typename Stream, typename StreamVisitor>
    friend std::result_of_t<StreamVisitor(Stream&)>
    with_serialized_stream(Stream& stream, StreamVisitor&& visitor);
};

// The purpose of the with_serialized_stream() is to minimize number of dynamic
// dispatches. For example, a lot of IDL-generated code looks like this:
// auto some_value() const {
//     return seastar::with_serialized_stream(v, [] (auto& v) {
//         auto in = v;
//         ser::skip(in, boost::type<type1>());
//         ser::skip(in, boost::type<type2>());
//         return deserialize(in, boost::type<type3>());
//     });
// }
// Using with_stream() there is at most one dynamic dispatch per such
// function, instead of one per each skip() and deserialize() call.

template<typename Stream, typename StreamVisitor>
[[gnu::always_inline]]
 static inline std::enable_if_t<Stream::has_with_stream::value, std::result_of_t<StreamVisitor(Stream&)>>
 with_serialized_stream(Stream& stream, StreamVisitor&& visitor) {
    return stream.with_stream(std::forward<StreamVisitor>(visitor));
}

template<typename Stream, typename StreamVisitor>
[[gnu::always_inline]]
 static inline std::enable_if_t<!Stream::has_with_stream::value, std::result_of_t<StreamVisitor(Stream&)>>
 with_serialized_stream(Stream& stream, StreamVisitor&& visitor) {
    return visitor(stream);
}

using simple_input_stream = memory_stream<simple_stream_tag>::simple;

}
