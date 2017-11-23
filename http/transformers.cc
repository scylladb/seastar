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
 * Copyright 2015 Cloudius Systems
 */

#include <boost/algorithm/string/replace.hpp>
#include "transformers.hh"
#include <experimental/string_view>
#include <list>

namespace seastar {

namespace httpd {

using namespace std;

struct potential_match_entry {
    const char* begin;
    const char* end;
    size_t pos;
};

/*!
 * \brief holds the buffer replace object current state
 * The way the matching algorithm works, is that when there's a match
 * it will be the first entry
 */
class buffer_replace_state {
    std::list<potential_match_entry> _potential_match;

public:
    using iterator = std::list<potential_match_entry>::iterator;

    void add_potential_match(const char* s, const char* e, size_t pos) {
        _potential_match.emplace_back(potential_match_entry{s, e, pos});
    }

    iterator begin() {
        return _potential_match.begin();
    }

    iterator end() {
        return _potential_match.end();
    }

    bool empty() const {
        return _potential_match.empty();
    }

    bool last() const {
        return _potential_match.size() == 1;
    }

    auto erase(const iterator& i) {
        return _potential_match.erase(i);
    }

    /*!
     * \brief gets the key/value position in the buffer_replace of the match
     */
    size_t get_pos() const {
        return (*_potential_match.begin()).pos;
    }

    /*!
     * \brief gets the length of the remaining string
     */
    size_t get_remaining_length() const {
        return _potential_match.begin()->end - _potential_match.begin()->begin;
    }

    void clear() {
        _potential_match.clear();
    }
};

/*!
 *\brief a helper class to replace strings in a buffer
 * The keys to replace are surrounded by braces
 */
class buffer_replace {
    std::vector<std::tuple<sstring, sstring>> _values;
    buffer_replace_state _current;
    const sstring& get_value(size_t pos) const;
    const sstring& get_key(size_t pos) const;
public:
    /*!
     * \brief Add a key and value to be replaced
     */
    buffer_replace& add(sstring key, sstring value) {
        _values.emplace_back(std::make_tuple("{{" + key + "}}", value));
        return *this;
    }

    /*!
     * \brief if there are no more buffers to consume get
     * the remaining chars stored in the buffer_replace
     */
    temporary_buffer<char> get_remaining();

    /*!
     * \brief check if the given buffer still match any of the current potential matches
     *
     */
    temporary_buffer<char> match(temporary_buffer<char>& buf);
    /*!
     * \brief replace the buffer content
     *
     * The returned result is after translation. The method consumes what it read
     * from the buf, so the caller  should check that buf is not empty.
     *
     * For example: if buf is: "abcd{{key}}"
     * res = replace(buf);
     *
     * res will be "abcd"
     * and buf will be "{{key}}"
     */
    temporary_buffer<char> replace(temporary_buffer<char>& buf);

    /*!
     * \brief check if we are currently in the middle of consuming
     */
    bool is_consuming() const {
        return !_current.empty();
    }
};


class content_replace_data_sink_impl : public data_sink_impl {
    output_stream<char> _out;
    buffer_replace _br;
public:
    content_replace_data_sink_impl(output_stream<char>&& out, std::vector<std::tuple<sstring,sstring>>&& key_value) : _out(std::move(out)) {
        for (auto& i : key_value) {
            _br.add(std::get<0>(i), std::get<1>(i));
        }
    }

    virtual future<> put(net::packet data)  override {
        return make_ready_future<>();
    }

    virtual future<> put(temporary_buffer<char> buf) override {
        if (buf.empty()) {
            return make_ready_future<>();
        }
        return do_with(temporary_buffer<char>(std::move(buf)), [this] (temporary_buffer<char>& buf) {
            return repeat([&buf, this] {
                auto bf = _br.replace(buf);
                return _out.write(bf.get(), bf.size()).then([&buf, this] {
                    return (buf.empty()) ? stop_iteration::yes : stop_iteration::no;
                });
            });
        });
    }

    virtual future<> flush() override {
        return _out.flush();
    }

    virtual future<> close() override {
        // if we are in the middle of a consuming a key
        // there will be no match, write the remaining.
        if (_br.is_consuming()) {
            return do_with(temporary_buffer<char>(_br.get_remaining()), [this](temporary_buffer<char>& buf) {
                return _out.write(buf.get(), buf.size()).then([this] {
                    return _out.flush();
                });
            });
        }
        return _out.flush();
    }
};

class content_replace_data_sink : public data_sink {
public:
    content_replace_data_sink(output_stream<char>&& out, std::vector<std::tuple<sstring,sstring>> key_value)
        : data_sink(std::make_unique<content_replace_data_sink_impl>(
                std::move(out), std::move(key_value))) {}
};

output_stream<char> content_replace::transform(std::unique_ptr<request> req,
            const sstring& extension, output_stream<char>&& s) {
    sstring host = req->get_header("Host");
    if (host == "" || (this->extension != "" && extension != this->extension)) {
        return std::move(s);
    }
    sstring protocol = req->get_protocol_name();
    return output_stream<char>(content_replace_data_sink(std::move(s), {std::make_tuple("Protocol", protocol), std::make_tuple("Host", host)}), 32000, true);

}

/*!
 * \brief find the open brace that surround a parameter
 * it is either two consecutive braces or a single brace, if it's the last char in the buffer
 */
ssize_t find_braces(const char* s, const char* end) {
    for (size_t i = 0; s != end; s++, i++) {
        if (*s == '{' && ((s + 1) == end || *(s + 1) == '{')) {
            return i;
        }
    }
    return -1;
}

const sstring& buffer_replace::get_value(size_t pos) const {
    return std::get<1>(_values[pos]);
}

const sstring& buffer_replace::get_key(size_t pos) const {
    return std::get<0>(_values[pos]);
}

temporary_buffer<char> buffer_replace::match(temporary_buffer<char>& buf) {
    if (_current.empty()) {
        return temporary_buffer<char>();
    }
    auto buf_len = buf.size();
    auto first = _current.begin();
    while (first != _current.end()) {
        auto& pos = first->begin;
        auto end = first->end;
        size_t len_compare = std::min(buf_len, static_cast<size_t>(end - pos));
        if (strncmp(pos, buf.begin(), len_compare)) {
            // No match remove the entry unless it's the last one
            // In that case, there is no match
            // we should return what we consumed so far;
            if (_current.last()) {
                auto res = get_remaining();
                _current.erase(first);
                return std::move(res);
            }
            first = _current.erase(first);
        } else {
            // we found a match
            if (pos + len_compare == end) {
                // this is a full match, there could be only one so this is the first
                // consume the buffer and return
                const sstring& value = get_value(_current.get_pos());
                temporary_buffer<char> res(value.data(), value.size());
                buf.trim_front(len_compare);
                _current.clear();
                return std::move(res);
            }
            // only partial match
            pos += len_compare;
            ++first;
        }
    }
    // if we are here we run out of buffer
    buf.trim_front(buf_len);
    return temporary_buffer<char>();
}

temporary_buffer<char> buffer_replace::get_remaining() {
    if (!is_consuming()) {
        return temporary_buffer<char>();
    }
    size_t pos = _current.get_pos();
    const sstring& key = get_key(pos);
    auto size = key.size() - _current.get_remaining_length();
    return temporary_buffer<char>(key.begin(), size);
}

temporary_buffer<char> buffer_replace::replace(temporary_buffer<char>& buf) {
    if (buf.empty()) {
        return std::move(buf);
    }
    if (is_consuming()) {
        return match(buf);
    }
    auto start = find_braces(buf.begin(), buf.end());
    if (start >= 0) {
        // we found an opening brace that is followed by a second brace or buffer end.
        // 1. All values are a possible match
        // 2. We can output the beginning of the buffer
        // 3. Need to continue matching the remaining buffer
        size_t pos = 0;
        for (auto&& i : _values) {
            sstring& key = std::get<0>(i);
            _current.add_potential_match(key.begin() + 1, key.end(), pos++);
        }
        temporary_buffer<char> res = buf.share(0, start);
        buf.trim_front(start + 1);
        return res;
    }

    return std::move(buf);
}

}

}
