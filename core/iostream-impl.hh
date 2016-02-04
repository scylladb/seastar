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
 * Copyright (C) 2015 Cloudius Systems, Ltd.
 */


#pragma once

#include "net/packet.hh"

namespace seastar {

template<typename CharType>
inline
future<> output_stream<CharType>::write(const char_type* buf) {
    return write(buf, strlen(buf));
}

template<typename CharType>
template<typename StringChar, typename SizeType, SizeType MaxSize>
inline
future<> output_stream<CharType>::write(const basic_sstring<StringChar, SizeType, MaxSize>& s) {
    return write(reinterpret_cast<const CharType *>(s.c_str()), s.size());
}

template<typename CharType>
inline
future<> output_stream<CharType>::write(const std::basic_string<CharType>& s) {
    return write(s.c_str(), s.size());
}

template<typename CharType>
future<> output_stream<CharType>::write(scattered_message<CharType> msg) {
    return write(std::move(msg).release());
}

template<typename CharType>
future<> output_stream<CharType>::write(net::packet p) {
    static_assert(std::is_same<CharType, char>::value, "packet works on char");

    if (p.len() == 0) {
        return make_ready_future<>();
    }

    assert(!_end && "Mixing buffered writes and zero-copy writes not supported yet");

    if (!_trim_to_size || p.len() <= _size) {
        // TODO: aggregate buffers for later coalescing. Currently we flush right
        // after appending the message anyway, so it doesn't matter.
        return _fd.put(std::move(p));
    }

    auto head = p.share(0, _size);
    p.trim_front(_size);
    return _fd.put(std::move(head)).then([this, p = std::move(p)] () mutable {
        return write(std::move(p));
    });
}

template<typename CharType>
future<> output_stream<CharType>::write(temporary_buffer<CharType> p) {
    if (p.empty()) {
        return make_ready_future<>();
    }
    assert(!_end && "Mixing buffered writes and zero-copy writes not supported yet");
    if (!_trim_to_size || p.size() <= _size) {
        // TODO: aggregate buffers for later coalescing.
        return _fd.put(std::move(p));
    }
    auto head = p.share(0, _size);
    p.trim_front(_size);
    return _fd.put(std::move(head)).then([this, p = std::move(p)] () mutable {
        return write(std::move(p));
    });
}

template <typename CharType>
future<temporary_buffer<CharType>>
input_stream<CharType>::read_exactly_part(size_t n, tmp_buf out, size_t completed) {
    if (available()) {
        auto now = std::min(n - completed, available());
        std::copy(_buf.get(), _buf.get() + now, out.get_write() + completed);
        _buf.trim_front(now);
        completed += now;
    }
    if (completed == n) {
        return make_ready_future<tmp_buf>(std::move(out));
    }

    // _buf is now empty
    return _fd.get().then([this, n, out = std::move(out), completed] (auto buf) mutable {
        if (buf.size() == 0) {
            _eof = true;
            return make_ready_future<tmp_buf>(std::move(buf));
        }
        _buf = std::move(buf);
        return this->read_exactly_part(n, std::move(out), completed);
    });
}

template <typename CharType>
future<temporary_buffer<CharType>>
input_stream<CharType>::read_exactly(size_t n) {
    if (_buf.size() == n) {
        // easy case: steal buffer, return to caller
        return make_ready_future<tmp_buf>(std::move(_buf));
    } else if (_buf.size() > n) {
        // buffer large enough, share it with caller
        auto front = _buf.share(0, n);
        _buf.trim_front(n);
        return make_ready_future<tmp_buf>(std::move(front));
    } else if (_buf.size() == 0) {
        // buffer is empty: grab one and retry
        return _fd.get().then([this, n] (auto buf) mutable {
            if (buf.size() == 0) {
                _eof = true;
                return make_ready_future<tmp_buf>(std::move(buf));
            }
            _buf = std::move(buf);
            return this->read_exactly(n);
        });
    } else {
        // buffer too small: start copy/read loop
        tmp_buf b(n);
        return read_exactly_part(n, std::move(b), 0);
    }
}

template <typename CharType>
template <typename Consumer>
future<>
input_stream<CharType>::consume(Consumer& consumer) {
    for (;;) {
        if (_buf.empty() && !_eof) {
            return _fd.get().then([this, &consumer] (tmp_buf buf) {
                _buf = std::move(buf);
                _eof = _buf.empty();
                return consume(consumer);
            });
        }
        future<unconsumed_remainder> unconsumed = consumer(std::move(_buf));
        if (unconsumed.available()) {
            unconsumed_remainder u = std::get<0>(unconsumed.get());
            if (u) {
                // consumer is done
                _buf = std::move(u.value());
                return make_ready_future<>();
            }
            if (_eof) {
                return make_ready_future<>();
            }
            // If we're here, consumer consumed entire buffer and is ready for
            // more now. So we do not return, and rather continue the loop.
            // TODO: if we did too many iterations, schedule a call to
            // consume() instead of continuing the loop.
        } else {
            // TODO: here we wait for the consumer to finish the previous
            // buffer (fulfilling "unconsumed") before starting to read the
            // next one. Consider reading ahead.
            return unconsumed.then([this, &consumer] (unconsumed_remainder u) {
                if (u) {
                    // consumer is done
                    _buf = std::move(u.value());
                    return make_ready_future<>();
                } else {
                    // consumer consumed entire buffer, and is ready for more
                    return consume(consumer);
                }
            });
        }
    }
}

template <typename CharType>
future<temporary_buffer<CharType>>
input_stream<CharType>::read() {
    using tmp_buf = temporary_buffer<CharType>;
    if (_eof) {
        return make_ready_future<tmp_buf>();
    }
    if (_buf.empty()) {
        return _fd.get().then([this] (tmp_buf buf) {
            _eof = buf.empty();
            return make_ready_future<tmp_buf>(std::move(buf));
        });
    } else {
        return make_ready_future<tmp_buf>(std::move(_buf));
    }
}

// Writes @buf in chunks of _size length. The last chunk is buffered if smaller.
template <typename CharType>
future<>
output_stream<CharType>::split_and_put(temporary_buffer<CharType> buf) {
    assert(_end == 0);

    if (buf.size() < _size) {
        if (!_buf) {
            _buf = _fd.allocate_buffer(_size);
        }
        std::copy(buf.get(), buf.get() + buf.size(), _buf.get_write());
        _end = buf.size();
        return make_ready_future<>();
    }

    auto chunk = buf.share(0, _size);
    buf.trim_front(_size);
    return put(std::move(chunk)).then([this, buf = std::move(buf)] () mutable {
        return split_and_put(std::move(buf));
    });
}

template <typename CharType>
future<>
output_stream<CharType>::write(const char_type* buf, size_t n) {
    auto bulk_threshold = _end ? (2 * _size - _end) : _size;
    if (n >= bulk_threshold) {
        if (_end) {
            auto now = _size - _end;
            std::copy(buf, buf + now, _buf.get_write() + _end);
            _end = _size;
            temporary_buffer<char> tmp = _fd.allocate_buffer(n - now);
            std::copy(buf + now, buf + n, tmp.get_write());
            _buf.trim(_end);
            _end = 0;
            return put(std::move(_buf)).then([this, tmp = std::move(tmp)]() mutable {
                if (_trim_to_size) {
                    return split_and_put(std::move(tmp));
                } else {
                    return put(std::move(tmp));
                }
            });
        } else {
            temporary_buffer<char> tmp = _fd.allocate_buffer(n);
            std::copy(buf, buf + n, tmp.get_write());
            if (_trim_to_size) {
                return split_and_put(std::move(tmp));
            } else {
                return put(std::move(tmp));
            }
        }
    }

    if (!_buf) {
        _buf = _fd.allocate_buffer(_size);
    }

    auto now = std::min(n, _size - _end);
    std::copy(buf, buf + now, _buf.get_write() + _end);
    _end += now;
    if (now == n) {
        return make_ready_future<>();
    } else {
        temporary_buffer<char> next = _fd.allocate_buffer(_size);
        std::copy(buf + now, buf + n, next.get_write());
        _end = n - now;
        std::swap(next, _buf);
        return put(std::move(next));
    }
}

template <typename CharType>
future<>
output_stream<CharType>::flush() {
    if (!_batch_flushes) {
        if (_end) {
            _buf.trim(_end);
            _end = 0;
            return put(std::move(_buf)).then([this] {
                return _fd.flush();
            });
        }
    } else {
        if (_ex) {
            // flush is a good time to deliver outstanding errors
            return make_exception_future<>(std::move(_ex));
        } else {
            _flush = true;
            if (!_in_batch) {
                add_to_flush_poller(this);
                _in_batch = promise<>();
            }
        }
    }
    return make_ready_future<>();
}

void add_to_flush_poller(output_stream<char>* x);

template <typename CharType>
future<>
output_stream<CharType>::put(temporary_buffer<CharType> buf) {
    // if flush is scheduled, disable it, so it will not try to write in parallel
    _flush = false;
    if (_flushing) {
        // flush in progress, wait for it to end before continuing
        return _in_batch.value().get_future().then([this, buf = std::move(buf)] () mutable {
            return _fd.put(std::move(buf));
        });
    } else {
        return _fd.put(std::move(buf));
    }
}

template <typename CharType>
void
output_stream<CharType>::poll_flush() {
    if (!_flush) {
        // flush was canceled, do nothing
        _flushing = false;
        _in_batch.value().set_value();
        _in_batch = std::experimental::nullopt;
        return;
    }

    auto f = make_ready_future();
    _flush = false;
    _flushing = true; // make whoever wants to write into the fd to wait for flush to complete

    if (_end) {
        // send whatever is in the buffer right now
        _buf.trim(_end);
        _end = 0;
        f = _fd.put(std::move(_buf));
    }

    f.then([this] {
        return _fd.flush();
    }).then_wrapped([this] (future<> f) {
        try {
            f.get();
        } catch (...) {
            _ex = std::current_exception();
        }
        // if flush() was called while flushing flush once more
        poll_flush();
    });
}

template <typename CharType>
future<>
output_stream<CharType>::close() {
    return flush().finally([this] {
        if (_in_batch) {
            return _in_batch.value().get_future();
        } else {
            return make_ready_future();
        }
    }).then([this] {
        // report final exception as close error
        if (_ex) {
            std::rethrow_exception(_ex);
        }
    }).finally([this] {
        return _fd.close();
    });
}

} // namespace seastar
