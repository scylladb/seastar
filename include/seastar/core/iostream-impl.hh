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

#include <seastar/core/coroutine.hh>
#include <seastar/core/do_with.hh>
#include <seastar/core/loop.hh>
#include <seastar/net/packet.hh>
#include <seastar/util/assert.hh>
#include <seastar/util/variant_utils.hh>

namespace seastar {

inline future<temporary_buffer<char>> data_source_impl::skip(uint64_t n)
{
    return do_with(uint64_t(n), [this] (uint64_t& n) {
        return repeat_until_value([&] {
            return get().then([&] (temporary_buffer<char> buffer) -> std::optional<temporary_buffer<char>> {
                if (buffer.empty()) {
                    return buffer;
                }
                if (buffer.size() >= n) {
                    buffer.trim_front(n);
                    return buffer;
                }
                n -= buffer.size();
                return { };
            });
        });
    });
}

template<typename CharType>
inline
future<> output_stream<CharType>::write(const char_type* buf) noexcept {
    return write(buf, strlen(buf));
}

template<typename CharType>
template<typename StringChar, typename SizeType, SizeType MaxSize, bool NulTerminate>
inline
future<> output_stream<CharType>::write(const basic_sstring<StringChar, SizeType, MaxSize, NulTerminate>& s) noexcept {
    return write(reinterpret_cast<const CharType *>(s.c_str()), s.size());
}

template<typename CharType>
inline
future<> output_stream<CharType>::write(const std::basic_string<CharType>& s) noexcept {
    return write(s.c_str(), s.size());
}

template<typename CharType>
future<> output_stream<CharType>::write(scattered_message<CharType> msg) noexcept {
    return write(std::move(msg).release());
}

template<typename CharType>
future<>
output_stream<CharType>::zero_copy_put(net::packet p) noexcept {
    // if flush is scheduled, disable it, so it will not try to write in parallel
    _flush = false;
    if (_flushing) {
        // flush in progress, wait for it to end before continuing
        return _in_batch.value().get_future().then([this, p = std::move(p)] () mutable {
            return _fd.put(std::move(p));
        });
    } else {
        return _fd.put(std::move(p));
    }
}

// Writes @p in chunks of _size length. The last chunk is buffered if smaller.
template <typename CharType>
future<>
output_stream<CharType>::zero_copy_split_and_put(net::packet p) noexcept {
    return repeat([this, p = std::move(p)] () mutable {
        if (p.len() < _size) {
            if (p.len()) {
                _zc_bufs = std::move(p);
            } else {
                _zc_bufs = net::packet::make_null_packet();
            }
            return make_ready_future<stop_iteration>(stop_iteration::yes);
        }
        auto chunk = p.share(0, _size);
        p.trim_front(_size);
        return zero_copy_put(std::move(chunk)).then([] {
            return stop_iteration::no;
        });
    });
}

template<typename CharType>
future<> output_stream<CharType>::write(net::packet p) noexcept {
    static_assert(std::is_same_v<CharType, char>, "packet works on char");
  try {
    if (p.len() != 0) {
        SEASTAR_ASSERT(!_end && "Mixing buffered writes and zero-copy writes not supported yet");

        if (_zc_bufs) {
            _zc_bufs.append(std::move(p));
        } else {
            _zc_bufs = std::move(p);
        }

        if (_zc_bufs.len() >= _size) {
            if (_trim_to_size) {
                return zero_copy_split_and_put(std::move(_zc_bufs));
            } else {
                return zero_copy_put(std::move(_zc_bufs));
            }
        }
    }
    return make_ready_future<>();
  } catch (...) {
    return current_exception_as_future();
  }
}

template<typename CharType>
future<> output_stream<CharType>::write(temporary_buffer<CharType> p) noexcept {
  try {
    if (p.empty()) {
        return make_ready_future<>();
    }
    SEASTAR_ASSERT(!_end && "Mixing buffered writes and zero-copy writes not supported yet");
    return write(net::packet(std::move(p)));
  } catch (...) {
    return current_exception_as_future();
  }
}

template <typename CharType>
future<temporary_buffer<CharType>>
input_stream<CharType>::read_exactly_part(size_t n) noexcept {
    temporary_buffer<CharType> out(n);
    size_t completed{0U};
    while (completed < n) {
        size_t avail = available();
        if (avail) {
            auto now = std::min(n - completed, avail);
            std::copy_n(_buf.get(), now, out.get_write() + completed);
            _buf.trim_front(now);
            completed += now;
            if (completed == n) {
                break;
            }
        }

        // _buf is now empty
        temporary_buffer<CharType> buf = co_await _fd.get();
        if (buf.size() == 0) {
            _eof = true;
            out.trim(completed);
            break;
        }
        _buf = std::move(buf);
    }
    co_return out;
}

template <typename CharType>
future<temporary_buffer<CharType>>
input_stream<CharType>::read_exactly(size_t n) noexcept {
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
        return read_exactly_part(n);
    }
}

template <typename CharType>
template <typename Consumer>
requires InputStreamConsumer<Consumer, CharType> || ObsoleteInputStreamConsumer<Consumer, CharType>
future<>
input_stream<CharType>::consume(Consumer&& consumer) noexcept(std::is_nothrow_move_constructible_v<Consumer>) {
    return repeat([consumer = std::move(consumer), this] () mutable {
        if (_buf.empty() && !_eof) {
            return _fd.get().then([this] (tmp_buf buf) {
                _buf = std::move(buf);
                _eof = _buf.empty();
                return make_ready_future<stop_iteration>(stop_iteration::no);
            });
        }
        return consumer(std::move(_buf)).then([this] (consumption_result_type result) {
            return seastar::visit(result.get(), [this] (const continue_consuming&) {
               // If we're here, consumer consumed entire buffer and is ready for
                // more now. So we do not return, and rather continue the loop.
                //
                // If we're at eof, we should stop.
                return make_ready_future<stop_iteration>(stop_iteration(this->_eof));
            }, [this] (stop_consuming<CharType>& stop) {
                // consumer is done
                this->_buf = std::move(stop.get_buffer());
                return make_ready_future<stop_iteration>(stop_iteration::yes);
            }, [this] (const skip_bytes& skip) {
                return this->_fd.skip(skip.get_value()).then([this](tmp_buf buf) {
                    if (!buf.empty()) {
                        this->_buf = std::move(buf);
                    }
                    return make_ready_future<stop_iteration>(stop_iteration::no);
                });
            });
        });
    });
}

template <typename CharType>
template <typename Consumer>
requires InputStreamConsumer<Consumer, CharType> || ObsoleteInputStreamConsumer<Consumer, CharType>
future<>
input_stream<CharType>::consume(Consumer& consumer) noexcept(std::is_nothrow_move_constructible_v<Consumer>) {
    return consume(std::ref(consumer));
}

template <typename CharType>
future<temporary_buffer<CharType>>
input_stream<CharType>::read_up_to(size_t n) noexcept {
    using tmp_buf = temporary_buffer<CharType>;
    if (_buf.empty()) {
        if (_eof) {
            return make_ready_future<tmp_buf>();
        } else {
            return _fd.get().then([this, n] (tmp_buf buf) {
                _eof = buf.empty();
                _buf = std::move(buf);
                return read_up_to(n);
            });
        }
    } else if (_buf.size() <= n) {
        // easy case: steal buffer, return to caller
        return make_ready_future<tmp_buf>(std::move(_buf));
    } else {
      try {
        // buffer is larger than n, so share its head with a caller
        auto front = _buf.share(0, n);
        _buf.trim_front(n);
        return make_ready_future<tmp_buf>(std::move(front));
      } catch (...) {
        return current_exception_as_future<tmp_buf>();
      }
    }
}

template <typename CharType>
future<temporary_buffer<CharType>>
input_stream<CharType>::read() noexcept {
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

template <typename CharType>
future<>
input_stream<CharType>::skip(uint64_t n) noexcept {
    auto skip_buf = std::min(n, _buf.size());
    _buf.trim_front(skip_buf);
    n -= skip_buf;
    if (!n) {
        return make_ready_future<>();
    }
    return _fd.skip(n).then([this] (temporary_buffer<CharType> buffer) {
        _buf = std::move(buffer);
    });
}

template <typename CharType>
data_source
input_stream<CharType>::detach() && {
    if (_buf) {
        throw std::logic_error("detach() called on a used input_stream");
    }

    return std::move(_fd);
}

// Writes @buf in chunks of _size length. The last chunk is buffered if smaller.
template <typename CharType>
future<>
output_stream<CharType>::split_and_put(temporary_buffer<CharType> buf) noexcept {
    SEASTAR_ASSERT(_end == 0);

    return repeat([this, buf = std::move(buf)] () mutable {
        if (buf.size() < _size) {
            if (!_buf) {
                _buf = _fd.allocate_buffer(_size);
            }
            std::copy(buf.get(), buf.get() + buf.size(), _buf.get_write());
            _end = buf.size();
            return make_ready_future<stop_iteration>(stop_iteration::yes);
        }
        auto chunk = buf.share(0, _size);
        buf.trim_front(_size);
        return put(std::move(chunk)).then([] {
            return stop_iteration::no;
        });
    });
}

template <typename CharType>
future<>
output_stream<CharType>::write(const char_type* buf, size_t n) noexcept {
    if (__builtin_expect(!_buf || n > _size - _end, false)) {
        return slow_write(buf, n);
    }
    std::copy_n(buf, n, _buf.get_write() + _end);
    _end += n;
    return make_ready_future<>();
}

template <typename CharType>
future<>
output_stream<CharType>::slow_write(const char_type* buf, size_t n) noexcept {
  try {
    SEASTAR_ASSERT(!_zc_bufs && "Mixing buffered writes and zero-copy writes not supported yet");
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
  } catch (...) {
    return current_exception_as_future();
  }
}

namespace internal {
void add_to_flush_poller(output_stream<char>& x) noexcept;
}

template <typename CharType>
future<> output_stream<CharType>::do_flush() noexcept {
    if (_end) {
        _buf.trim(_end);
        _end = 0;
        return _fd.put(std::move(_buf)).then([this] {
            return _fd.flush();
        });
    } else if (_zc_bufs) {
        return _fd.put(std::move(_zc_bufs)).then([this] {
            return _fd.flush();
        });
    } else {
        return _fd.flush();
    }
}

template <typename CharType>
future<>
output_stream<CharType>::flush() noexcept {
    if (!_batch_flushes) {
        return do_flush();
    } else {
        if (_ex) {
            // flush is a good time to deliver outstanding errors
            return make_exception_future<>(std::move(_ex));
        } else {
            _flush = true;
            if (!_in_batch) {
                internal::add_to_flush_poller(*this);
                _in_batch = promise<>();
            }
        }
    }
    return make_ready_future<>();
}

template <typename CharType>
future<>
output_stream<CharType>::put(temporary_buffer<CharType> buf) noexcept {
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
output_stream<CharType>::poll_flush() noexcept {
    if (!_flush) {
        // flush was canceled, do nothing
        _flushing = false;
        _in_batch.value().set_value();
        _in_batch = std::nullopt;
        return;
    }

    _flush = false;
    _flushing = true; // make whoever wants to write into the fd to wait for flush to complete

    // FIXME: future is discarded
    (void)do_flush().then_wrapped([this] (future<> f) {
        try {
            f.get();
        } catch (...) {
            _ex = std::current_exception();
            _fd.on_batch_flush_error();
        }
        // if flush() was called while flushing flush once more
        poll_flush();
    });
}

template <typename CharType>
future<>
output_stream<CharType>::close() noexcept {
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

template <typename CharType>
data_sink
output_stream<CharType>::detach() && {
    if (_buf) {
        throw std::logic_error("detach() called on a used output_stream");
    }

    return std::move(_fd);
}

namespace internal {

/// \cond internal
template <typename CharType>
struct stream_copy_consumer {
private:
    output_stream<CharType>& _os;
    using consumption_result_type = consumption_result<CharType>;
public:
    stream_copy_consumer(output_stream<CharType>& os) : _os(os) {
    }
    future<consumption_result_type> operator()(temporary_buffer<CharType> data) {
        if (data.empty()) {
            return make_ready_future<consumption_result_type>(stop_consuming(std::move(data)));
        }
        return _os.write(data.get(), data.size()).then([] {
            return make_ready_future<consumption_result_type>(continue_consuming());
        });
    }
};
/// \endcond

}

extern template struct internal::stream_copy_consumer<char>;

template <typename CharType>
future<> copy(input_stream<CharType>& in, output_stream<CharType>& out) {
    return in.consume(internal::stream_copy_consumer<CharType>(out));
}

extern template future<> copy<char>(input_stream<char>&, output_stream<char>&);
}
