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

#include "future.hh"
#include "temporary_buffer.hh"
#include "scattered_message.hh"

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
    // Consumer concept, for consume() method:
    using unconsumed_remainder = std::experimental::optional<tmp_buf>;
    struct ConsumerConcept {
        // The consumer should operate on the data given to it, and
        // return a future "unconsumed remainder", which can be undefined
        // if the consumer consumed all the input given to it and is ready
        // for more, or defined when the consumer is done (and in that case
        // the value is the unconsumed part of the last data buffer - this
        // can also happen to be empty).
        future<unconsumed_remainder> operator()(tmp_buf data);
    };
    using char_type = CharType;
    input_stream() = default;
    explicit input_stream(data_source fd) : _fd(std::move(fd)), _buf(0) {}
    input_stream(input_stream&&) = default;
    input_stream& operator=(input_stream&&) = default;
    future<temporary_buffer<CharType>> read_exactly(size_t n);
    template <typename Consumer> future<> consume(Consumer&& c);
    template <typename Consumer> future<> consume(Consumer& c);
    bool eof() { return _eof; }
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

// Facilitates data buffering before it's handed over to data_sink.
//
// When trim_to_size is true it's guaranteed that data sink will not receive
// chunks larger than the configured size, which could be the case when a
// single write call is made with data larger than the configured size.
//
// The data sink will not receive empty chunks.
//
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
    std::experimental::optional<promise<>> _in_batch;
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
public:
    using char_type = CharType;
    output_stream() = default;
    output_stream(data_sink fd, size_t size, bool trim_to_size = false, bool batch_flushes = false)
        : _fd(std::move(fd)), _size(size), _trim_to_size(trim_to_size), _batch_flushes(batch_flushes) {}
    output_stream(output_stream&&) = default;
    output_stream& operator=(output_stream&&) = default;
    ~output_stream() { assert(!_in_batch); }
    future<> write(const char_type* buf, size_t n);
    future<> write(const char_type* buf);

    template <typename StringChar, typename SizeType, SizeType MaxSize>
    future<> write(const basic_sstring<StringChar, SizeType, MaxSize>& s);
    future<> write(const std::basic_string<char_type>& s);

    future<> write(net::packet p);
    future<> write(scattered_message<char_type> msg);
    future<> write(temporary_buffer<char_type>);
    future<> flush();
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

}

#include "iostream-impl.hh"
