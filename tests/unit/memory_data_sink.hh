
#pragma once

#include <seastar/core/future.hh>
#include <seastar/core/temporary_buffer.hh>
#include <seastar/net/api.hh>
#include <seastar/core/iostream.hh>

#include <sstream>

namespace seastar {
/*!
 * \brief a helper data sink that stores everything it gets in a stringstream
 */
class memory_data_sink_impl : public data_sink_impl {
    std::stringstream& _ss;
    size_t _buffer_size;
public:

    static constexpr size_t default_buffer_size = 1024;

    memory_data_sink_impl(std::stringstream& ss, size_t buffer_size = default_buffer_size) :
        _ss(ss),
        _buffer_size(buffer_size) {}

#if SEASTAR_API_LEVEL >= 9
    future<> put(std::span<temporary_buffer<char>> bufs) override {
        for (auto& buf : bufs) {
            _ss.write(buf.get(), buf.size());
        }
        return make_ready_future<>();
    }
#else
    virtual future<> put(net::packet data)  override {
        return data_sink_impl::fallback_put(std::move(data));
    }
    virtual future<> put(temporary_buffer<char> buf) override {
        _ss.write(buf.get(), buf.size());
        return make_ready_future<>();
    }
#endif
    virtual future<> flush() override {
        return make_ready_future<>();
    }

    virtual future<> close() override {
        return make_ready_future<>();
    }

    virtual size_t buffer_size() const noexcept override {
        return _buffer_size;
    }
};

class memory_data_sink : public data_sink {
public:
    memory_data_sink(std::stringstream& ss)
        : data_sink(std::make_unique<memory_data_sink_impl>(ss)) {}
};

}
