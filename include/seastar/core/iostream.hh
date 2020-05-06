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
#include <seastar/core/temporary_buffer.hh>
#include <seastar/core/scattered_message.hh>
#include <seastar/util/std-compat.hh>

namespace seastar {

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
    data_source() = default;
    explicit data_source(std::unique_ptr<data_source_impl> dsi) : _dsi(std::move(dsi)) {}
    data_source(data_source&& x) = default;
    data_source& operator=(data_source&& x) = default;
    future<temporary_buffer<char>> get() { return _dsi->get(); }
    future<temporary_buffer<char>> skip(uint64_t n) { return _dsi->skip(n); }
    future<> close() { return _dsi->close(); }
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
};

class data_sink {
    std::unique_ptr<data_sink_impl> _dsi;
public:
    data_sink() = default;
    explicit data_sink(std::unique_ptr<data_sink_impl> dsi) : _dsi(std::move(dsi)) {}
    data_sink(data_sink&& x) = default;
    data_sink& operator=(data_sink&& x) = default;
    temporary_buffer<char> allocate_buffer(size_t size) {
        return _dsi->allocate_buffer(size);
    }
    future<> put(std::vector<temporary_buffer<char>> data) {
        return _dsi->put(std::move(data));
    }
    future<> put(temporary_buffer<char> data) {
        return _dsi->put(std::move(data));
    }
    future<> put(net::packet p) {
        return _dsi->put(std::move(p));
    }
    future<> flush() {
        return _dsi->flush();
    }
    future<> close() { return _dsi->close(); }
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
    using consumption_variant = compat::variant<continue_consuming, stop_consuming_type, skip_bytes>;
    using tmp_buf = typename stop_consuming_type::tmp_buf;

    /*[[deprecated]]*/ consumption_result(compat::optional<tmp_buf> opt_buf) {
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
SEASTAR_CONCEPT(
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
    { c(temporary_buffer<CharType>{}) } -> std::same_as<future<compat::optional<temporary_buffer<CharType>>>>;
};
)

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
    size_t available() const { return _buf.size(); }
protected:
    void reset() { _buf = {}; }
    data_source* fd() { return &_fd; }
public:
    using consumption_result_type = consumption_result<CharType>;
    // unconsumed_remainder is mapped for compatibility only; new code should use consumption_result_type
    using unconsumed_remainder = compat::optional<tmp_buf>;
    using char_type = CharType;
    input_stream() = default;
    explicit input_stream(data_source fd) : _fd(std::move(fd)), _buf(0) {}
    input_stream(input_stream&&) = default;
    input_stream& operator=(input_stream&&) = default;
    future<temporary_buffer<CharType>> read_exactly(size_t n);
    template <typename Consumer>
    SEASTAR_CONCEPT(requires InputStreamConsumer<Consumer, CharType> || ObsoleteInputStreamConsumer<Consumer, CharType>)
    future<> consume(Consumer&& c);
    template <typename Consumer>
    SEASTAR_CONCEPT(requires InputStreamConsumer<Consumer, CharType> || ObsoleteInputStreamConsumer<Consumer, CharType>)
    future<> consume(Consumer& c);
    bool eof() const { return _eof; }
    /// Returns some data from the stream, or an empty buffer on end of
    /// stream.
    future<tmp_buf> read();
    /// Returns up to n bytes from the stream, or an empty buffer on end of
    /// stream.
    future<tmp_buf> read_up_to(size_t n);
    /// Detaches the \c input_stream from the underlying data source.
    ///
    /// Waits for any background operations (for example, read-ahead) to
    /// complete, so that the any resources the stream is using can be
    /// safely destroyed.  An example is a \ref file resource used by
    /// the stream returned by make_file_input_stream().
    ///
    /// \return a future that becomes ready when this stream no longer
    ///         needs the data source.
    future<> close() {
        return _fd.close();
    }
    /// Ignores n next bytes from the stream.
    future<> skip(uint64_t n);

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
    future<temporary_buffer<CharType>> read_exactly_part(size_t n, tmp_buf buf, size_t completed);
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
    compat::optional<promise<>> _in_batch;
    bool _flush = false;
    bool _flushing = false;
    std::exception_ptr _ex;
private:
    size_t available() const { return _end - _begin; }
    size_t possibly_available() const { return _size - _begin; }
    future<> split_and_put(temporary_buffer<CharType> buf);
    future<> put(temporary_buffer<CharType> buf);
    void poll_flush();
    future<> zero_copy_put(net::packet p);
    future<> zero_copy_split_and_put(net::packet p);
    [[gnu::noinline]]
    future<> slow_write(const CharType* buf, size_t n);
public:
    using char_type = CharType;
    output_stream() = default;
    output_stream(data_sink fd, size_t size, bool trim_to_size = false, bool batch_flushes = false)
        : _fd(std::move(fd)), _size(size), _trim_to_size(trim_to_size), _batch_flushes(batch_flushes) {}
    output_stream(output_stream&&) = default;
    output_stream& operator=(output_stream&&) = default;
    ~output_stream() { assert(!_in_batch && "Was this stream properly closed?"); }
    future<> write(const char_type* buf, size_t n);
    future<> write(const char_type* buf);

    template <typename StringChar, typename SizeType, SizeType MaxSize, bool NulTerminate>
    future<> write(const basic_sstring<StringChar, SizeType, MaxSize, NulTerminate>& s);
    future<> write(const std::basic_string<char_type>& s);

    future<> write(net::packet p);
    future<> write(scattered_message<char_type> msg);
    future<> write(temporary_buffer<char_type>);
    future<> flush();

    /// Flushes the stream before closing it (and the underlying data sink) to
    /// any further writes.  The resulting future must be waited on before
    /// destroying this object.
    future<> close();

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
private:
    friend class reactor;
};

/*!
 * \brief copy all the content from the input stream to the output stream
 */
template <typename CharType>
future<> copy(input_stream<CharType>&, output_stream<CharType>&);

}

#include "iostream-impl.hh"
