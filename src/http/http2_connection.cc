#include "seastar/core/future.hh"
#include <seastar/http/http2_connection.hh>
#include <seastar/core/loop.hh>
#include <seastar/core/do_with.hh>
#include <seastar/core/when_all.hh>
#include <seastar/core/print.hh>
#include <seastar/core/future-util.hh>
#include <seastar/core/temporary_buffer.hh>
#include <seastar/core/iostream.hh>
#include <seastar/util/log.hh>
#include <cstring>

namespace seastar {
namespace http {

static logger h2log("http2");

future<> http2_connection::process() {
    h2log.info("Starting HTTP/2 connection processing");
    return read_loop();
}

future<> http2_connection::read_loop() {
    h2log.info("Entering HTTP/2 read loop");
    return do_until([this] { return _done; }, [this] {
        return read_one_frame();
    });
}

future<> http2_connection::read_one_frame() {
    // Read 9-byte HTTP/2 frame header
    // According to the HTTP/2 spec, the frame header is always 9 bytes long.
    // https://datatracker.ietf.org/doc/html/rfc7540#section-4.1
    return _read_buf.read_exactly(9).then([this](temporary_buffer<char> hdr_buf) {
        if (hdr_buf.size() < 9) {
            h2log.error("Connection closed or incomplete frame header");
            _done = true;
            return make_ready_future<>();
        }
        // Parse the frame header
        _parser.init();
        _parser.parse(hdr_buf.get_write(), hdr_buf.get_write() + 9);
        uint32_t frame_len = _parser._length;
        uint8_t frame_type = _parser._type;
        //TODO: parse frame flags and streamid
        h2log.info("Frame: type={} len={}", frame_type, frame_len);
        _done = true;
        // Depending on the the frame type, call the appropriate handler
        return make_ready_future<>();
    });
}

} // namespace http
} // namespace seastar 