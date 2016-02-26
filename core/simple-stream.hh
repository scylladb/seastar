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

class simple_input_stream {
    const char* _p;
    size_t _size;
public:
    simple_input_stream(const simple_input_stream& o) : _p(o._p), _size(o._size) {}
    simple_input_stream(const char* p, size_t size) : _p(p), _size(size) {}
    const char* begin() const { return _p; }
    void skip(size_t size) {
        if (size > _size) {
            throw std::out_of_range("deserialization buffer underflow");
        }
        _p += size;
        _size -= size;
    }
    simple_input_stream read_substream(size_t size) {
       if (size > _size) {
           throw std::out_of_range("deserialization buffer underflow");
       }
       simple_input_stream substream(_p, size);
       skip(size);
       return substream;
    }
    void read(char* p, size_t size) {
        if (size > _size) {
            throw std::out_of_range("deserialization buffer underflow");
        }
        std::copy_n(_p, size, p);
        skip(size);
    }
    const size_t size() const {
        return _size;
    }
};

}
