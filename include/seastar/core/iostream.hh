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

//
// Buffered input and output streams
//
// Two abstract classes (data_source and data_sink) provide means
// to acquire bulk data from, or push bulk data to, some provider.
// These could be tied to a TCP connection, a disk file, or a memory
// buffer.
//
// Two concrete classes (input_stream and output_stream) buffer data
// from data_source and data_sink and provide easier means to process
// it.
//

#pragma once

#include <seastar/core/future.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/temporary_buffer.hh>
#include <seastar/core/scattered_message.hh>
#include <seastar/util/assert.hh>
#include <seastar/util/std-compat.hh>
#include <seastar/util/modules.hh>
#ifndef SEASTAR_MODULE
#include <boost/intrusive/slist.hpp>
#include <algorithm>
#include <memory>
#include <optional>
#include <variant>
#include <vector>
#endif

namespace bi = boost::intrusive;

namespace seastar {

SEASTAR_MODULE_EXPORT_BEGIN

namespace net { class packet; }

class data_source_impl {
public:
    virtual ~data_source_impl() {}
    virtual future<temporary_buffer<char>> get() = 0;
    virtual future<temporary_buffer<char>> skip(uint64_t n);
    virtual future<> close() { return make_ready_future<>(); }
};

class data_source {
    std::unique_ptr<data_source_impl> _dsi;
protected:
    data_source_impl* impl() const { return _dsi.get(); }
public:
    using tmp_buf = temporary_buffer<char>;

    data_source() noexcept = default;
    explicit data_source(std::unique_ptr<data_source_impl> dsi) noexcept : _dsi(std::move(dsi)) {}
    data_source(data_source&& x) noexcept = default;
    data_source& operator=(data_source&& x) noexcept = default;

    future<tmp_buf> get() noexcept {
        try {
            return _dsi->get();
        } catch (...) {
            return current_exception_as_future<tmp_buf>();
        }
    }
    future<tmp_buf> skip(uint64_t n) noexcept {
        try {
            return _dsi->skip(n);
        } catch (...) {
            return current_exception_as_future<tmp_buf>();
        }
    }
    future<> close() noexcept {
        try {
            return _dsi->close();
        } catch (...) {
            return current_exception_as_future<>();
        }
    }
};

class data_sink_impl {
public:
    virtual ~data_sink_impl() {}
    virtual temporary_buffer<char> allocate_buffer(size_t size) {
        return temporary_buffer<char>(size);
    }
    virtual future<> put(net::packet data) = 0;
    virtual future<> put(std::vector<temporary_buffer<char>> data) {
        net::packet p;
        p.reserve(data.size());
        for (auto& buf : data) {
            p = net::packet(std::move(p), net::fragment{buf.get_write(), buf.size()}, buf.release());
        }
        return put(std::move(p));
    }
    virtual future<> put(temporary_buffer<char> buf) {
        return put(net::packet(net::fragment{buf.get_write(), buf.size()}, buf.release()));
    }
    virtual future<> flush() {
        return make_ready_future<>();
    }
    virtual future<> close() = 0;

    // The method should return the maximum buffer size that's acceptable by
    // the sink. It's used when the output stream is constructed without any
    // specific buffer size. In this case the stream accepts this value as its
    // buffer size and doesn't put larger buffers (see trim_to_size).
    virtual size_t buffer_size() const noexcept {
        SEASTAR_ASSERT(false && "Data sink must have the buffer_size() method overload");
        return 0;
    }

    // In order to support flushes batching (output_stream_options.batch_flushes)
    // the sink mush handle flush errors that may happen in the background by
    // overriding the on_batch_flush_error() method. If the sink doesn't do it,
    // turning on batch_flushes would have no effect
    virtual bool can_batch_flushes() const noexcept {
        return false;
    }

    virtual void on_batch_flush_error() noexcept {
        SEASTAR_ASSERT(false && "Data sink must implement on_batch_flush_error() method");
    }

protected:
    // This is a helper function that classes that inherit from data_sink_impl
    // can use to implement the put overload for net::packet.
    // Unfortunately, we currently cannot define this function as
    // 'virtual future<> put(net::packet)', because we would get infinite
    // recursion between this function and
    // 'virtual future<> put(temporary_buffer<char>)'.
    future<> fallback_put(net::packet data) {
        auto buffers = data.release();
        for (temporary_buffer<char>& buf : buffers) {
            co_await this->put(std::move(buf));
        }
    }
};

class data_sink {
    std::unique_ptr<data_sink_impl> _dsi;
public:
    data_sink() noexcept = default;
    explicit data_sink(std::unique_ptr<data_sink_impl> dsi) noexcept : _dsi(std::move(dsi)) {}
    data_sink(data_sink&& x) noexcept = default;
    data_sink& operator=(data_sink&& x) noexcept = default;
    temporary_buffer<char> allocate_buffer(size_t size) {
        return _dsi->allocate_buffer(size);
    }
    future<> put(std::vector<temporary_buffer<char>> data) noexcept {
      try {
        return _dsi->put(std::move(data));
      } catch (...) {
        return current_exception_as_future();
      }
    }
    future<> put(temporary_buffer<char> data) noexcept {
      try {
        return _dsi->put(std::move(data));
      } catch (...) {
        return current_exception_as_future();
      }
    }
    future<> put(net::packet p) noexcept {
      try {
        return _dsi->put(std::move(p));
      } catch (...) {
        return current_exception_as_future();
      }
    }
    future<> flush() noexcept {
      try {
        return _dsi->flush();
      } catch (...) {
        return current_exception_as_future();
      }
    }
    future<> close() noexcept {
        try {
            return _dsi->close();
        } catch (...) {
            return current_exception_as_future();
        }
    }

    size_t buffer_size() const noexcept { return _dsi->buffer_size(); }
    bool can_batch_flushes() const noexcept { return _dsi->can_batch_flushes(); }
    void on_batch_flush_error() noexcept { _dsi->on_batch_flush_error(); }
};

struct continue_consuming {};

template <typename CharType>
class stop_consuming {
public:
    using tmp_buf = temporary_buffer<CharType>;
    explicit stop_consuming(tmp_buf buf) : _buf(std::move(buf)) {}

    tmp_buf& get_buffer() { return _buf; }
    const tmp_buf& get_buffer() const { return _buf; }
private:
    tmp_buf _buf;
};

class skip_bytes {
public:
    explicit skip_bytes(uint64_t v) : _value(v) {}
    uint64_t get_value() const { return _value; }
private:
    uint64_t _value;
};

template <typename CharType>
class consumption_result {
public:
    using stop_consuming_type = stop_consuming<CharType>;
    using consumption_variant = std::variant<continue_consuming, stop_consuming_type, skip_bytes>;
    using tmp_buf = typename stop_consuming_type::tmp_buf;

    /*[[deprecated]]*/ consumption_result(std::optional<tmp_buf> opt_buf) {
        if (opt_buf) {
            _result = stop_consuming_type{std::move(opt_buf.value())};
        }
    }

    consumption_result(const continue_consuming&) {}
    consumption_result(stop_consuming_type&& stop) : _result(std::move(stop)) {}
    consumption_result(skip_bytes&& skip) : _result(std::move(skip)) {}

    consumption_variant& get() { return _result; }
    const consumption_variant& get() const { return _result; }

private:
    consumption_variant _result;
};

// Consumer concept, for consume() method

// The consumer should operate on the data given to it, and
// return a future "consumption result", which can be
//  - continue_consuming, if the consumer has consumed all the input given
// to it and is ready for more
//  - stop_consuming, when the consumer is done (and in that case
// the contained buffer is the unconsumed part of the last data buffer - this
// can also happen to be empty).
//  - skip_bytes, when the consumer has consumed all the input given to it
// and wants to skip before processing the next chunk
//
// For backward compatibility reasons, we also support the deprecated return value
// of type "unconsumed remainder" which can be
//  - empty optional, if the consumer consumed all the input given to it
// and is ready for more
//  - non-empty optional, when the consumer is done (and in that case
// the value is the unconsumed part of the last data buffer - this
// can also happen to be empty).

template <typename Consumer, typename CharType>
concept InputStreamConsumer = requires (Consumer c) {
    { c(temporary_buffer<CharType>{}) } -> std::same_as<future<consumption_result<CharType>>>;
};

template <typename Consumer, typename CharType>
concept ObsoleteInputStreamConsumer = requires (Consumer c) {
    { c(temporary_buffer<CharType>{}) } -> std::same_as<future<std::optional<temporary_buffer<CharType>>>>;
};

/// Buffers data from a data_source and provides a stream interface to the user.
///
/// \note All methods must be called sequentially.  That is, no method may be
/// invoked before the previous method's returned future is resolved.
template <typename CharType>
class input_stream final {
    static_assert(sizeof(CharType) == 1, "must buffer stream of bytes");
    data_source _fd;
    temporary_buffer<CharType> _buf;
    bool _eof = false;
private:
    using tmp_buf = temporary_buffer<CharType>;
    size_t available() const noexcept { return _buf.size(); }
protected:
    void reset() noexcept { _buf = {}; }
    data_source* fd() noexcept { return &_fd; }
public:
    using consumption_result_type = consumption_result<CharType>;
    // unconsumed_remainder is mapped for compatibility only; new code should use consumption_result_type
    using unconsumed_remainder = std::optional<tmp_buf>;
    using char_type = CharType;
    input_stream() noexcept = default;
    explicit input_stream(data_source fd) noexcept : _fd(std::move(fd)), _buf() {}
    input_stream(input_stream&&) = default;
    input_stream& operator=(input_stream&&) = default;
    /// Reads n bytes from the stream, or fewer if reached the end of stream.
    ///
    /// \returns a future that waits until n bytes are available in the
    /// stream and returns them. If the end of stream is reached before n
    /// bytes were read, fewer than n bytes will be returned - so despite
    /// the method's name, the caller must not assume the returned buffer
    /// will always contain exactly n bytes.
    ///
    /// \throws if an I/O error occurs during the read. As explained above,
    /// prematurely reaching the end of stream is *not* an I/O error.
    future<temporary_buffer<CharType>> read_exactly(size_t n) noexcept;
    template <typename Consumer>
    requires InputStreamConsumer<Consumer, CharType> || ObsoleteInputStreamConsumer<Consumer, CharType>
    future<> consume(Consumer&& c) noexcept(std::is_nothrow_move_constructible_v<Consumer>);
    template <typename Consumer>
    requires InputStreamConsumer<Consumer, CharType> || ObsoleteInputStreamConsumer<Consumer, CharType>
    future<> consume(Consumer& c) noexcept(std::is_nothrow_move_constructible_v<Consumer>);
    /// Returns true if the end-of-file flag is set on the stream.
    /// Note that the eof flag is only set after a previous attempt to read
    /// from the stream noticed the end of the stream. In other words, it is
    /// possible that eof() returns false but read() will return an empty
    /// buffer. Checking eof() again after the read will return true.
    bool eof() const noexcept { return _eof; }
    /// Returns some data from the stream, or an empty buffer on end of
    /// stream.
    future<tmp_buf> read() noexcept;
    /// Returns up to n bytes from the stream, or an empty buffer on end of
    /// stream.
    future<tmp_buf> read_up_to(size_t n) noexcept;
    /// Detaches the \c input_stream from the underlying data source.
    ///
    /// Waits for any background operations (for example, read-ahead) to
    /// complete, so that the any resources the stream is using can be
    /// safely destroyed.  An example is a \ref file resource used by
    /// the stream returned by make_file_input_stream().
    ///
    /// \return a future that becomes ready when this stream no longer
    ///         needs the data source.
    future<> close() noexcept {
        return _fd.close();
    }
    /// Ignores n next bytes from the stream.
    future<> skip(uint64_t n) noexcept;

    /// Detaches the underlying \c data_source from the \c input_stream.
    ///
    /// The intended usage is custom \c data_source_impl implementations
    /// wrapping an existing \c input_stream, therefore it shouldn't be
    /// called on an \c input_stream that was already used.
    /// After calling \c detach() the \c input_stream is in an unusable,
    /// moved-from state.
    ///
    /// \throws std::logic_error if called on a used stream
    ///
    /// \returns the data_source
    data_source detach() &&;
private:
    future<temporary_buffer<CharType>> read_exactly_part(size_t n) noexcept;
};

struct output_stream_options {
    bool trim_to_size = false; ///< Make sure that buffers put into sink haven't
                               ///< grown larger than the configured size
    bool batch_flushes = false; ///< Try to merge flushes with each other
};

/// Facilitates data buffering before it's handed over to data_sink.
///
/// When trim_to_size is true it's guaranteed that data sink will not receive
/// chunks larger than the configured size, which could be the case when a
/// single write call is made with data larger than the configured size.
///
/// The data sink will not receive empty chunks.
///
/// \note All methods must be called sequentially.  That is, no method
/// may be invoked before the previous method's returned future is
/// resolved.
template <typename CharType>
class output_stream final {
    static_assert(sizeof(CharType) == 1, "must buffer stream of bytes");
    data_sink _fd;
    temporary_buffer<CharType> _buf;
    net::packet _zc_bufs = net::packet::make_null_packet(); //zero copy buffers
    size_t _size = 0;
    size_t _begin = 0;
    size_t _end = 0;
    bool _trim_to_size = false;
    bool _batch_flushes = false;
    std::optional<promise<>> _in_batch;
    bool _flush = false;
    bool _flushing = false;
    std::exception_ptr _ex;
    bi::slist_member_hook<> _in_poller;

private:
    size_t available() const noexcept { return _end - _begin; }
    future<> split_and_put(temporary_buffer<CharType> buf) noexcept;
    future<> put(temporary_buffer<CharType> buf) noexcept;
    void poll_flush() noexcept;
    future<> do_flush() noexcept;
    future<> zero_copy_put(net::packet p) noexcept;
    future<> zero_copy_split_and_put(net::packet p) noexcept;
    [[gnu::noinline]]
    future<> slow_write(const CharType* buf, size_t n) noexcept;
public:
    using char_type = CharType;
    output_stream() noexcept = default;
    output_stream(data_sink fd, size_t size, output_stream_options opts = {}) noexcept
        : _fd(std::move(fd)), _size(size), _trim_to_size(opts.trim_to_size), _batch_flushes(opts.batch_flushes && _fd.can_batch_flushes()) {}
    [[deprecated("use output_stream_options instead of booleans")]]
    output_stream(data_sink fd, size_t size, bool trim_to_size, bool batch_flushes = false) noexcept
        : _fd(std::move(fd)), _size(size), _trim_to_size(trim_to_size), _batch_flushes(batch_flushes && _fd.can_batch_flushes()) {}
    output_stream(data_sink fd) noexcept
        : _fd(std::move(fd)), _size(_fd.buffer_size()), _trim_to_size(true) {}
    output_stream(output_stream&&) noexcept = default;
    output_stream& operator=(output_stream&&) noexcept = default;
    ~output_stream() {
        if (_batch_flushes) {
            SEASTAR_ASSERT(!_in_batch && "Was this stream properly closed?");
        } else {
            SEASTAR_ASSERT(!_end && !_zc_bufs && "Was this stream properly closed?");
        }
    }
    future<> write(const char_type* buf, size_t n) noexcept;
    future<> write(const char_type* buf) noexcept;

    template <typename StringChar, typename SizeType, SizeType MaxSize, bool NulTerminate>
    future<> write(const basic_sstring<StringChar, SizeType, MaxSize, NulTerminate>& s) noexcept;
    future<> write(const std::basic_string<char_type>& s) noexcept;

    future<> write(net::packet p) noexcept;
    future<> write(scattered_message<char_type> msg) noexcept;
    future<> write(temporary_buffer<char_type>) noexcept;
    future<> flush() noexcept;

    /// Flushes the stream before closing it (and the underlying data sink) to
    /// any further writes.  The resulting future must be waited on before
    /// destroying this object.
    future<> close() noexcept;

    /// Detaches the underlying \c data_sink from the \c output_stream.
    ///
    /// The intended usage is custom \c data_sink_impl implementations
    /// wrapping an existing \c output_stream, therefore it shouldn't be
    /// called on an \c output_stream that was already used.
    /// After calling \c detach() the \c output_stream is in an unusable,
    /// moved-from state.
    ///
    /// \throws std::logic_error if called on a used stream
    ///
    /// \returns the data_sink
    data_sink detach() &&;

    using batch_flush_list_t = bi::slist<output_stream,
            bi::constant_time_size<false>, bi::cache_last<true>,
            bi::member_hook<output_stream, bi::slist_member_hook<>, &output_stream::_in_poller>>;
private:
    friend class reactor;
};

/*!
 * \brief copy all the content from the input stream to the output stream
 */
template <typename CharType>
future<> copy(input_stream<CharType>&, output_stream<CharType>&);

SEASTAR_MODULE_EXPORT_END
}

#include "iostream-impl.hh"
