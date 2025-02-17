#include <seastar/rpc/rpc.hh>
#include <seastar/rpc/multi_algo_compressor_factory.hh>
#include <seastar/core/align.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/print.hh>
#include <seastar/core/future-util.hh>
#include <seastar/core/metrics.hh>
#include <seastar/util/assert.hh>
#include <boost/range/adaptor/map.hpp>
#include <boost/range/adaptor/transformed.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/range/numeric.hpp>
#include <fmt/ostream.h>

#if FMT_VERSION >= 90000
template <> struct fmt::formatter<seastar::rpc::streaming_domain_type> : fmt::ostream_formatter {};
#endif

namespace seastar {

namespace rpc {

    void logger::operator()(const client_info& info, id_type msg_id, const sstring& str) const {
        log(format("client {} msg_id {}:  {}", info.addr, msg_id, str));
    }

    void logger::operator()(const client_info& info, id_type msg_id, log_level level, std::string_view str) const {
        log(level, "client {} msg_id {}:  {}", info.addr, msg_id, str);
    }

    void logger::operator()(const client_info& info, const sstring& str) const {
        (*this)(info.addr, str);
    }

    void logger::operator()(const client_info& info, log_level level, std::string_view str) const {
        (*this)(info.addr, level, str);
    }

    void logger::operator()(const socket_address& addr, const sstring& str) const {
        log(format("client {}: {}", addr, str));
    }

    void logger::operator()(const socket_address& addr, log_level level, std::string_view str) const {
        log(level, "client {}: {}", addr, str);
    }

  no_wait_type no_wait;

  snd_buf::snd_buf(size_t size_) : size(size_) {
      if (size <= chunk_size) {
          bufs = temporary_buffer<char>(size);
      } else {
          std::vector<temporary_buffer<char>> v;
          v.reserve(align_up(size_t(size), chunk_size) / chunk_size);
          while (size_) {
              v.emplace_back(std::min(chunk_size, size_));
              size_ -= v.back().size();
          }
          bufs = std::move(v);
      }
  }

  snd_buf::snd_buf(snd_buf&&) noexcept = default;
  snd_buf& snd_buf::operator=(snd_buf&&) noexcept = default;

  temporary_buffer<char>& snd_buf::front() {
      auto* one = std::get_if<temporary_buffer<char>>(&bufs);
      if (one) {
          return *one;
      } else {
          return std::get<std::vector<temporary_buffer<char>>>(bufs).front();
      }
  }

  // Make a copy of a remote buffer. No data is actually copied, only pointers and
  // a deleter of a new buffer takes care of deleting the original buffer
  template<typename T> // T is either snd_buf or rcv_buf
  T make_shard_local_buffer_copy(foreign_ptr<std::unique_ptr<T>> org) {
      if (org.get_owner_shard() == this_shard_id()) {
          return std::move(*org);
      }
      T buf(org->size);
      auto* one = std::get_if<temporary_buffer<char>>(&org->bufs);

      if (one) {
          buf.bufs = temporary_buffer<char>(one->get_write(), one->size(), make_object_deleter(std::move(org)));
      } else {
          auto& orgbufs = std::get<std::vector<temporary_buffer<char>>>(org->bufs);
          std::vector<temporary_buffer<char>> newbufs;
          newbufs.reserve(orgbufs.size());
          deleter d = make_object_deleter(std::move(org));
          for (auto&& b : orgbufs) {
              newbufs.emplace_back(b.get_write(), b.size(), d.share());
          }
          buf.bufs = std::move(newbufs);
      }

      return buf;
  }

  template snd_buf make_shard_local_buffer_copy(foreign_ptr<std::unique_ptr<snd_buf>>);
  template rcv_buf make_shard_local_buffer_copy(foreign_ptr<std::unique_ptr<rcv_buf>>);

  static void log_exception(connection& c, log_level level, const char* log, std::exception_ptr eptr) {
      const char* s;
      try {
          std::rethrow_exception(eptr);
      } catch (std::exception& ex) {
          s = ex.what();
      } catch (...) {
          s = "unknown exception";
      }
      auto formatted = format("{}: {}", log, s);
      c.get_logger()(c.peer_address(), level, std::string_view(formatted.data(), formatted.size()));
  }

  snd_buf connection::compress(snd_buf buf) {
      if (_compressor) {
          buf = _compressor->compress(4, std::move(buf));
          static_assert(snd_buf::chunk_size >= 4, "send buffer chunk size is too small");
          write_le<uint32_t>(buf.front().get_write(), buf.size - 4);
          return buf;
      }
      return buf;
  }

  future<> connection::send_buffer(snd_buf buf) {
      auto* b = std::get_if<temporary_buffer<char>>(&buf.bufs);
      if (b) {
          return _write_buf.write(std::move(*b));
      } else {
          return do_with(std::move(std::get<std::vector<temporary_buffer<char>>>(buf.bufs)),
                  [this] (std::vector<temporary_buffer<char>>& ar) {
              return do_for_each(ar.begin(), ar.end(), [this] (auto& b) {
                  return _write_buf.write(std::move(b));
              });
          });
      }
  }

  future<> connection::send_entry(outgoing_entry& d) noexcept {
    return futurize_invoke([this, &d] {
      if (d.buf.size && _propagate_timeout) {
          static_assert(snd_buf::chunk_size >= sizeof(uint64_t), "send buffer chunk size is too small");
          if (_timeout_negotiated) {
              auto expire = d.t.get_timeout();
              uint64_t left = 0;
              if (expire != typename timer<rpc_clock_type>::time_point()) {
                  left = std::chrono::duration_cast<std::chrono::milliseconds>(expire - timer<rpc_clock_type>::clock::now()).count();
              }
              write_le<uint64_t>(d.buf.front().get_write(), left);
          } else {
              d.buf.front().trim_front(sizeof(uint64_t));
              d.buf.size -= sizeof(uint64_t);
          }
      }
      auto buf = compress(std::move(d.buf));
      return send_buffer(std::move(buf)).then([this] {
          _stats.sent_messages++;
          return _write_buf.flush();
      });
    });
  }

  void connection::set_negotiated() noexcept {
      _negotiated->set_value();
      _negotiated = std::nullopt;
  }

  future<> connection::stop_send_loop(std::exception_ptr ex) {
      _error = true;
      if (_connected) {
          _fd.shutdown_output();
      }
      if (ex == nullptr) {
          ex = std::make_exception_ptr(closed_error());
      }
      while (!_outgoing_queue.empty()) {
          auto it = std::prev(_outgoing_queue.end());
          // Cancel all but front entry normally. The front entry is sitting in the
          // send_entry() and cannot be withdrawn, except when _negotiated is still
          // engaged. In the latter case when it will be aborted below the entry's
          // continuation will not be called and its done promise will not resolve
          // the _outgoing_queue_ready, so do it here
          if (it != _outgoing_queue.begin()) {
              withdraw(it, ex);
          } else {
              if (_negotiated) {
                  it->done.set_exception(ex);
              }
              break;
          }
      }
      if (_negotiated) {
          _negotiated->set_exception(ex);
      }
      return when_all(std::move(_outgoing_queue_ready), std::move(_sink_closed_future)).then([this] (std::tuple<future<>, future<bool>> res){
          // _outgoing_queue_ready might be exceptional if queue drain or
          // _negotiated abortion set it such
          std::get<0>(res).ignore_ready_future();
          // _sink_closed_future is never exceptional
          bool sink_closed = std::get<1>(res).get();
          return _connected && !sink_closed ? _write_buf.close() : make_ready_future();
      });
  }

  void connection::set_socket(connected_socket&& fd) {
      if (_connected) {
          throw std::runtime_error("already connected");
      }
      _fd = std::move(fd);
      _read_buf =_fd.input();
      _write_buf = _fd.output();
      _connected = true;
  }

  future<> connection::send_negotiation_frame(feature_map features) {
      auto negotiation_frame_feature_record_size = [] (const feature_map::value_type& e) {
          return 8 + e.second.size();
      };
      auto extra_len = boost::accumulate(
              features | boost::adaptors::transformed(negotiation_frame_feature_record_size),
              uint32_t(0));
      temporary_buffer<char> reply(sizeof(negotiation_frame) + extra_len);
      auto p = reply.get_write();
      p = std::copy_n(rpc_magic, 8, p);
      write_le<uint32_t>(p, extra_len);
      p += 4;
      for (auto&& e : features) {
          write_le<uint32_t>(p, static_cast<uint32_t>(e.first));
          p += 4;
          write_le<uint32_t>(p, e.second.size());
          p += 4;
          p = std::copy_n(e.second.begin(), e.second.size(), p);
      }
      return _write_buf.write(std::move(reply)).then([this] {
          _stats.sent_messages++;
          return _write_buf.flush();
      });
  }

  void connection::withdraw(outgoing_entry::container_t::iterator it, std::exception_ptr ex) {
      SEASTAR_ASSERT(it != _outgoing_queue.end());

      auto pit = std::prev(it);
      // Previous entry's (pit's) done future will schedule current entry (it)
      // continuation. Similarly, it.done will schedule next entry continuation
      // or will resolve _outgoing_queue_ready future.
      //
      // To withdraw "it" we need to do two things:
      // - make pit.done resolve it->next (some time later)
      // - resolve "it"'s continuation right now
      //
      // The latter is achieved by resolving pit.done immediatelly, the former
      // by moving it.done into pit.done. For simplicity (verging on obscurity?)
      // both done's are just swapped and "it" resolves its new promise

      std::swap(it->done, pit->done);
      it->uncancellable();
      it->unlink();
      if (ex == nullptr) {
          it->done.set_value();
      } else {
          it->done.set_exception(ex);
      }
  }

  future<> connection::send(snd_buf buf, std::optional<rpc_clock_type::time_point> timeout, cancellable* cancel) {
      if (!_error) {
          if (timeout && *timeout <= rpc_clock_type::now()) {
              return make_ready_future<>();
          }

          auto p = std::make_unique<outgoing_entry>(std::move(buf));
          auto& d = *p;
          _outgoing_queue.push_back(d);
          _outgoing_queue_size++;
          auto deleter = [this, it = _outgoing_queue.iterator_to(d)] {
              // Front entry is most likely (unless _negotiated is unresolved, check enqueue_zero_frame()) sitting
              // inside send_entry() continuations and thus it cannot be cancelled.
              if (it != _outgoing_queue.begin()) {
                  withdraw(it);
              }
          };

          if (timeout) {
              auto& t = d.t;
              t.set_callback(deleter);
              t.arm(timeout.value());
          }
          if (cancel) {
              cancel->cancel_send = std::move(deleter);
              cancel->send_back_pointer = &d.pcancel;
              d.pcancel = cancel;
          }

          // New entry should continue (do its .then() lambda) after _outgoing_queue_ready
          // resolves. Next entry will need to do the same after this entry's done resolves.
          // Thus -- replace _outgoing_queue_ready with d's future and chain its continuation
          // on ..._ready's old value.
          return std::exchange(_outgoing_queue_ready, d.done.get_future()).then([this, p = std::move(p)] () mutable {
              _outgoing_queue_size--;
              if (__builtin_expect(!p->is_linked(), false)) {
                  // If withdrawn the entry is unlinked and this lambda is fired right at once
                  return make_ready_future<>();
              }

              p->uncancellable();
              return send_entry(*p).then_wrapped([this, p = std::move(p)] (auto f) mutable {
                  if (f.failed()) {
                      f.ignore_ready_future();
                      abort();
                  }
                  p->done.set_value();
              });
          });
      } else {
          return make_exception_future<>(closed_error());
      }
  }

  void connection::abort() {
      if (!_error) {
          _error = true;
          _fd.shutdown_input();
      }
  }

  future<> connection::stop() noexcept {
      try {
          abort();
      } catch (...) {
          log_exception(*this, log_level::error, "fail to shutdown connection while stopping", std::current_exception());
      }
      return _stopped.get_future();
  }

  template<typename Connection>
  static bool verify_frame(Connection& c, temporary_buffer<char>& buf, size_t expected, const char* log) {
      if (buf.size() != expected) {
          if (buf.size() != 0) {
              c.get_logger()(c.peer_address(), log);
          }
          return false;
      }
      return true;
  }

  template<typename Connection>
  static
  future<feature_map>
  receive_negotiation_frame(Connection& c, input_stream<char>& in) {
      return in.read_exactly(sizeof(negotiation_frame)).then([&c, &in] (temporary_buffer<char> neg) {
          if (!verify_frame(c, neg, sizeof(negotiation_frame), "unexpected eof during negotiation frame")) {
              return make_exception_future<feature_map>(closed_error());
          }
          negotiation_frame frame;
          std::copy_n(neg.get_write(), sizeof(frame.magic), frame.magic);
          frame.len = read_le<uint32_t>(neg.get_write() + 8);
          if (std::memcmp(frame.magic, rpc_magic, sizeof(frame.magic)) != 0) {
              c.get_logger()(c.peer_address(), format("wrong protocol magic: {:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}",
                    frame.magic[0], frame.magic[1], frame.magic[2], frame.magic[3], frame.magic[4], frame.magic[5], frame.magic[6], frame.magic[7]));
              return make_exception_future<feature_map>(closed_error());
          }
          auto len = frame.len;
          return in.read_exactly(len).then([&c, len] (temporary_buffer<char> extra) {
              if (extra.size() != len) {
                  c.get_logger()(c.peer_address(), "unexpected eof during negotiation frame");
                  return make_exception_future<feature_map>(closed_error());
              }
              feature_map map;
              auto p = extra.get();
              auto end = p + extra.size();
              while (p != end) {
                  if (end - p < 8) {
                      c.get_logger()(c.peer_address(), "bad feature data format in negotiation frame");
                      return make_exception_future<feature_map>(closed_error());
                  }
                  auto feature = static_cast<protocol_features>(read_le<uint32_t>(p));
                  auto f_len = read_le<uint32_t>(p + 4);
                  p += 8;
                  if (f_len > end - p) {
                      c.get_logger()(c.peer_address(), "buffer underflow in feature data in negotiation frame");
                      return make_exception_future<feature_map>(closed_error());
                  }
                  auto data = sstring(p, f_len);
                  p += f_len;
                  map.emplace(feature, std::move(data));
              }
              return make_ready_future<feature_map>(std::move(map));
          });
      });
  }

  inline future<rcv_buf>
  read_rcv_buf(input_stream<char>& in, uint32_t size) {
      return in.read_up_to(size).then([&, size] (temporary_buffer<char> data) mutable {
          rcv_buf rb(size);
          if (data.size() == 0) {
              return make_ready_future<rcv_buf>(rcv_buf());
          } else if (data.size() == size) {
              rb.bufs = std::move(data);
              return make_ready_future<rcv_buf>(std::move(rb));
          } else {
              size -= data.size();
              std::vector<temporary_buffer<char>> v;
              v.push_back(std::move(data));
              rb.bufs = std::move(v);
              return do_with(std::move(rb), std::move(size), [&in] (rcv_buf& rb, uint32_t& left) {
                  return repeat([&] () {
                      return in.read_up_to(left).then([&] (temporary_buffer<char> data) {
                          if (!data.size()) {
                              rb.size -= left;
                              return stop_iteration::yes;
                          } else {
                              left -= data.size();
                              std::get<std::vector<temporary_buffer<char>>>(rb.bufs).push_back(std::move(data));
                              return left ? stop_iteration::no : stop_iteration::yes;
                          }
                      });
                  }).then([&rb] {
                      return std::move(rb);
                  });
              });
          }
      });
  }

  template<typename FrameType>
  future<typename FrameType::return_type>
  connection::read_frame(socket_address info, input_stream<char>& in) {
      auto header_size = FrameType::header_size();
      return in.read_exactly(header_size).then([this, header_size, info, &in] (temporary_buffer<char> header) {
          if (header.size() != header_size) {
              if (header.size() != 0) {
                  _logger(info, format("unexpected eof on a {} while reading header: expected {:d} got {:d}", FrameType::role(), header_size, header.size()));
              }
              return make_ready_future<typename FrameType::return_type>(FrameType::empty_value());
          }
          auto [size, h] = FrameType::decode_header(header.get());
          if (!size) {
              return make_ready_future<typename FrameType::return_type>(FrameType::make_value(h, rcv_buf()));
          } else {
              return read_rcv_buf(in, size).then([this, info, h = std::move(h), size] (rcv_buf rb) {
                  if (rb.size != size) {
                      _logger(info, format("unexpected eof on a {} while reading data: expected {:d} got {:d}", FrameType::role(), size, rb.size));
                      return make_ready_future<typename FrameType::return_type>(FrameType::empty_value());
                  } else {
                      return make_ready_future<typename FrameType::return_type>(FrameType::make_value(h, std::move(rb)));
                  }
              });
          }
      });
  }

  template<typename FrameType>
  future<typename FrameType::return_type>
  connection::read_frame_compressed(socket_address info, std::unique_ptr<compressor>& compressor, input_stream<char>& in) {
      if (compressor) {
          return in.read_exactly(4).then([this, info, &in, &compressor] (temporary_buffer<char> compress_header) {
              if (compress_header.size() != 4) {
                  if (compress_header.size() != 0) {
                      _logger(info, format("unexpected eof on a {} while reading compression header: expected 4 got {:d}", FrameType::role(), compress_header.size()));
                  }
                  return make_ready_future<typename FrameType::return_type>(FrameType::empty_value());
              }
              auto ptr = compress_header.get();
              auto size = read_le<uint32_t>(ptr);
              return read_rcv_buf(in, size).then([this, size, &compressor, info, &in] (rcv_buf compressed_data) {
                  if (compressed_data.size != size) {
                      _logger(info, format("unexpected eof on a {} while reading compressed data: expected {:d} got {:d}", FrameType::role(), size, compressed_data.size));
                      return make_ready_future<typename FrameType::return_type>(FrameType::empty_value());
                  }
                  auto eb = compressor->decompress(std::move(compressed_data));
                  if (eb.size == 0) {
                      // Empty frames might be sent as means of communication between the compressors, and should be skipped by the RPC layer.
                      // We skip the empty frame here. We recursively restart the function, as if the empty frame didn't happen.
                      // The yield() is here to limit the stack depth of the recursion to 1.
                      return yield().then([this, info, &in, &compressor] { return read_frame_compressed<FrameType>(info, compressor, in); });
                  }
                  net::packet p;
                  auto* one = std::get_if<temporary_buffer<char>>(&eb.bufs);
                  if (one) {
                      p = net::packet(std::move(p), std::move(*one));
                  } else {
                      auto&& bufs = std::get<std::vector<temporary_buffer<char>>>(eb.bufs);
                      p.reserve(bufs.size());
                      for (auto&& b : bufs) {
                          p = net::packet(std::move(p), std::move(b));
                      }
                  }
                  return do_with(as_input_stream(std::move(p)), [this, info] (input_stream<char>& in) {
                      return read_frame<FrameType>(info, in);
                  });
              });
          });
      } else {
          return read_frame<FrameType>(info, in);
      }
  }

  struct stream_frame {
      using opt_buf_type = std::optional<rcv_buf>;
      using return_type = opt_buf_type;
      struct header_type {
          bool eos;
      };
      static size_t header_size() {
          return 4;
      }
      static const char* role() {
          return "stream";
      }
      static auto empty_value() {
          return std::nullopt;
      }
      static std::pair<uint32_t, header_type> decode_header(const char* ptr) {
          auto size = read_le<uint32_t>(ptr);
          return size != -1U ? std::make_pair(size, header_type{false}) : std::make_pair(0U, header_type{true});
      }
      static auto make_value(const header_type& t, rcv_buf data) {
          if (t.eos) {
              data.size = -1U;
          }
          return data;
      }
  };

  future<std::optional<rcv_buf>>
  connection::read_stream_frame_compressed(input_stream<char>& in) {
      return read_frame_compressed<stream_frame>(peer_address(), _compressor, in);
  }

  future<> connection::stream_close() {
      auto f = make_ready_future<>();
      if (!error()) {
          promise<bool> p;
          _sink_closed_future = p.get_future();
          // stop_send_loop(), which also calls _write_buf.close(), and this code can run in parallel.
          // Use _sink_closed_future to serialize them and skip second call to close()
          f = _write_buf.close().finally([p = std::move(p)] () mutable { p.set_value(true);});
      }
      return f.finally([this] () mutable { return stop(); });
  }

  future<> connection::stream_process_incoming(rcv_buf&& buf) {
      // we do not want to dead lock on huge packets, so let them in
      // but only one at a time
      auto size = std::min(size_t(buf.size), max_stream_buffers_memory);
      return get_units(_stream_sem, size).then([this, buf = std::move(buf)] (semaphore_units<>&& su) mutable {
          buf.su = std::move(su);
          return _stream_queue.push_eventually(std::move(buf));
      });
  }

  future<> connection::handle_stream_frame() {
      return read_stream_frame_compressed(_read_buf).then([this] (std::optional<rcv_buf> data) {
          if (!data) {
              _error = true;
              return make_ready_future<>();
          }
          return stream_process_incoming(std::move(*data));
      });
  }

  future<> connection::stream_receive(circular_buffer<foreign_ptr<std::unique_ptr<rcv_buf>>>& bufs) {
      return _stream_queue.not_empty().then([this, &bufs] {
          bool eof = !_stream_queue.consume([&bufs] (rcv_buf&& b) {
              if (b.size == -1U) { // max fragment length marks an end of a stream
                  return false;
              } else {
                  bufs.push_back(make_foreign(std::make_unique<rcv_buf>(std::move(b))));
                  return true;
              }
          });
          if (eof && !bufs.empty()) {
              SEASTAR_ASSERT(_stream_queue.empty());
              _stream_queue.push(rcv_buf(-1U)); // push eof marker back for next read to notice it
          }
      });
  }

  void connection::register_stream(connection_id id, xshard_connection_ptr c) {
      _streams.emplace(id, std::move(c));
  }

  xshard_connection_ptr connection::get_stream(connection_id id) const {
      auto it = _streams.find(id);
      if (it == _streams.end()) {
          throw std::logic_error(format("rpc stream id {} not found", id).c_str());
      }
      return it->second;
  }

  // The request frame is
  //   le64 optional timeout (see request_frame_with_timeout below)
  //   le64 message type a.k.a. verb ID
  //   le64 message ID
  //   le32 payload length
  //   ...  payload
  struct request_frame {
      using opt_buf_type = std::optional<rcv_buf>;
      using return_type = std::tuple<std::optional<uint64_t>, uint64_t, int64_t, opt_buf_type>;
      using header_type = std::tuple<std::optional<uint64_t>, uint64_t, int64_t>;
      static constexpr size_t raw_header_size = sizeof(uint64_t) + sizeof(int64_t) + sizeof(uint32_t);
      static size_t header_size() {
          static_assert(request_frame_headroom >= raw_header_size);
          return raw_header_size;
      }
      static const char* role() {
          return "server";
      }
      static auto empty_value() {
          return std::make_tuple(std::nullopt, uint64_t(0), 0, std::nullopt);
      }
      static std::pair<size_t, header_type> decode_header(const char* ptr) {
          auto type = read_le<uint64_t>(ptr);
          auto msgid = read_le<int64_t>(ptr + 8);
          auto size = read_le<uint32_t>(ptr + 16);
          return std::make_pair(size, std::make_tuple(std::nullopt, type, msgid));
      }
      static void encode_header(uint64_t type, int64_t msg_id, snd_buf& buf, size_t off) {
          auto p = buf.front().get_write() + off;
          write_le<uint64_t>(p, type);
          write_le<int64_t>(p + 8, msg_id);
          write_le<uint32_t>(p + 16, buf.size - raw_header_size - off);
      }
      static auto make_value(const header_type& t, rcv_buf data) {
          return std::make_tuple(std::get<0>(t), std::get<1>(t), std::get<2>(t), std::move(data));
      }
  };

  // This frame is used if protocol_features.TIMEOUT was negotiated
  struct request_frame_with_timeout : request_frame {
      using super = request_frame;
      static constexpr size_t raw_header_size = sizeof(uint64_t) + request_frame::raw_header_size;
      static size_t header_size() {
          static_assert(request_frame_headroom >= raw_header_size);
          return raw_header_size;
      }
      static std::pair<uint32_t, typename super::header_type> decode_header(const char* ptr) {
          auto h = super::decode_header(ptr + 8);
          std::get<0>(h.second) = read_le<uint64_t>(ptr);
          return h;
      }
      static void encode_header(uint64_t type, int64_t msg_id, snd_buf& buf) {
          static_assert(snd_buf::chunk_size >= raw_header_size, "send buffer chunk size is too small");
          // expiration timer is encoded later
          request_frame::encode_header(type, msg_id, buf, 8);
      }
  };

  future<> client::request(uint64_t type, int64_t msg_id, snd_buf buf, std::optional<rpc_clock_type::time_point> timeout, cancellable* cancel) {
      request_frame_with_timeout::encode_header(type, msg_id, buf);
      return send(std::move(buf), timeout, cancel);
  }

  void
  client::negotiate(feature_map provided) {
      // record features returned here
      for (auto&& e : provided) {
          auto id = e.first;
          switch (id) {
          // supported features go here
          case protocol_features::COMPRESS:
              if (_options.compressor_factory) {
                  _compressor = _options.compressor_factory->negotiate(e.second, false, [this] { return send({}); });
              }
              if (!_compressor) {
                  throw std::runtime_error(format("RPC server responded with compression {} - unsupported", e.second));
              }
              break;
          case protocol_features::TIMEOUT:
              _timeout_negotiated = true;
              break;
          case protocol_features::HANDLER_DURATION:
              _handler_duration_negotiated = true;
              break;
          case protocol_features::CONNECTION_ID: {
              _id = deserialize_connection_id(e.second);
              break;
          }
          default:
              // nothing to do
              ;
          }
      }
  }

  future<> client::negotiate_protocol(feature_map features) {
      return send_negotiation_frame(std::move(features)).then([this] {
          return receive_negotiation_frame(*this, _read_buf).then([this] (feature_map features) {
              return negotiate(std::move(features));
          });
      });
  }

  // The response frame is
  //   le64 message ID
  //   le32 payload size
  //   ...  payload
  struct response_frame {
      using opt_buf_type = std::optional<rcv_buf>;
      using return_type = std::tuple<int64_t, std::optional<uint32_t>, opt_buf_type>;
      using header_type = std::tuple<int64_t, std::optional<uint32_t>>;
      static constexpr size_t raw_header_size = sizeof(int64_t) + sizeof(uint32_t);
      static size_t header_size() {
          static_assert(response_frame_headroom >= raw_header_size);
          return raw_header_size;
      }
      static const char* role() {
          return "client";
      }
      static auto empty_value() {
          return std::make_tuple(0, std::nullopt, std::nullopt);
      }
      static std::pair<uint32_t, header_type> decode_header(const char* ptr) {
          auto msgid = read_le<int64_t>(ptr);
          auto size = read_le<uint32_t>(ptr + 8);
          return std::make_pair(size, std::make_tuple(msgid, std::nullopt));
      }
      static void encode_header(int64_t msg_id, snd_buf& data, size_t header_size = raw_header_size) {
          static_assert(snd_buf::chunk_size >= raw_header_size, "send buffer chunk size is too small");
          auto p = data.front().get_write();
          write_le<int64_t>(p, msg_id);
          write_le<uint32_t>(p + 8, data.size - header_size);
      }
      static auto make_value(const header_type& t, rcv_buf data) {
          return std::make_tuple(std::get<0>(t), std::get<1>(t), std::move(data));
      }
  };

  // The response frame is
  //   le64 message ID
  //   le32 payload size
  //   le32 handler duration
  //   ...  payload
  struct response_frame_with_handler_time : public response_frame {
      using super = response_frame;
      static constexpr size_t raw_header_size = super::raw_header_size + sizeof(uint32_t);
      static size_t header_size() {
          static_assert(response_frame_headroom >= raw_header_size);
          return raw_header_size;
      }
      static std::pair<uint32_t, header_type> decode_header(const char* ptr) {
          auto p = super::decode_header(ptr);
          auto ht = read_le<uint32_t>(ptr + 12);
          if (ht != -1U) {
              std::get<1>(p.second) = ht;
          }
          return p;
      }
      static uint32_t encode_handler_duration(const std::optional<rpc_clock_type::duration>& ht) noexcept {
          if (ht.has_value()) {
              std::chrono::microseconds us = std::chrono::duration_cast<std::chrono::microseconds>(*ht);
              if (us.count() < std::numeric_limits<uint32_t>::max()) {
                  return us.count();
              }
          }
          return -1U;
      }
      static void encode_header(int64_t msg_id, std::optional<rpc_clock_type::duration> ht, snd_buf& data) {
          static_assert(snd_buf::chunk_size >= raw_header_size);
          auto p = data.front().get_write();
          super::encode_header(msg_id, data, raw_header_size);
          write_le<uint32_t>(p + 12, encode_handler_duration(ht));
      }
  };

  future<response_frame::return_type>
  client::read_response_frame_compressed(input_stream<char>& in) {
      if (_handler_duration_negotiated) {
          return read_frame_compressed<response_frame_with_handler_time>(_server_addr, _compressor, in);
      } else {
          return read_frame_compressed<response_frame>(_server_addr, _compressor, in);
      }
  }

  stats client::get_stats() const {
      stats res = _stats;
      res.wait_reply = incoming_queue_length();
      res.pending = outgoing_queue_length();
      return res;
  }

  void client::wait_for_reply(id_type id, std::unique_ptr<reply_handler_base>&& h, std::optional<rpc_clock_type::time_point> timeout, cancellable* cancel) {
      if (timeout) {
          h->t.set_callback(std::bind(std::mem_fn(&client::wait_timed_out), this, id));
          h->t.arm(timeout.value());
      }
      if (cancel) {
          cancel->cancel_wait = [this, id] {
              _outstanding[id]->cancel();
              _outstanding.erase(id);
          };
          h->pcancel = cancel;
          cancel->wait_back_pointer = &h->pcancel;
      }
      _outstanding.emplace(id, std::move(h));
  }
  void client::wait_timed_out(id_type id) {
      _stats.timeout++;
      _outstanding[id]->timeout();
      _outstanding.erase(id);
  }

  future<> client::stop() noexcept {
      _error = true;
      try {
          _socket.shutdown();
      } catch(...) {
          log_exception(*this, log_level::error, "fail to shutdown connection while stopping", std::current_exception());
      }
      return _stopped.get_future();
  }

  void client::abort_all_streams() {
      while (!_streams.empty()) {
          auto&& s = _streams.begin();
          SEASTAR_ASSERT(s->second->get_owner_shard() == this_shard_id()); // abort can be called only locally
          s->second->get()->abort();
          _streams.erase(s);
      }
  }

  void client::deregister_this_stream() {
      if (_parent) {
          _parent->_streams.erase(_id);
      }
  }

  // This is the enlightened copy of the connection::send() method. Its intention is to
  // keep a dummy entry in front of the queue while connect+negotiate is happenning so
  // that all subsequent entries could abort on timeout or explicit cancellation.
  void client::enqueue_zero_frame() {
      if (_error) {
          return;
      }

      auto p = std::make_unique<outgoing_entry>(snd_buf(0));
      auto& d = *p;
      _outgoing_queue.push_back(d);

      // Make it in the background. Even if the client is stopped it will pick
      // up all the entries hanging around
      (void)std::exchange(_outgoing_queue_ready, d.done.get_future()).then_wrapped([p = std::move(p)] (auto f) mutable {
          if (f.failed()) {
              f.ignore_ready_future();
          } else {
              p->done.set_value();
          }
      });
  }

  struct client::metrics::domain {
      metrics::domain_list_t list;
      stats dead;
      seastar::metrics::metric_groups metric_groups;

      static thread_local std::unordered_map<sstring, domain> all;
      static domain& find_or_create(sstring name);

      stats::counter_type count_all(stats::counter_type stats::* field) noexcept {
          stats::counter_type res = dead.*field;
          for (const auto& m : list) {
              res += m._c._stats.*field;
          }
          return res;
      }

      size_t count_all_fn(size_t (client::*fn)(void) const) noexcept {
          size_t res = 0;
          for (const auto& m : list) {
              res += (m._c.*fn)();
          }
          return res;
      }

      domain(sstring name)
      {
          namespace sm = seastar::metrics;
          auto domain_l = sm::label("domain")(name);

          metric_groups.add_group("rpc_client", {
                sm::make_gauge("count", [this] { return list.size(); },
                        sm::description("Total number of clients"), { domain_l }),
                sm::make_counter("sent_messages", std::bind(&domain::count_all, this, &stats::sent_messages),
                        sm::description("Total number of messages sent"), { domain_l }),
                sm::make_counter("replied", std::bind(&domain::count_all, this, &stats::replied),
                        sm::description("Total number of responses received"), { domain_l }),
                sm::make_counter("exception_received", std::bind(&domain::count_all, this, &stats::exception_received),
                        sm::description("Total number of exceptional responses received"), { domain_l }).set_skip_when_empty(),
                sm::make_counter("timeout", std::bind(&domain::count_all, this, &stats::timeout),
                        sm::description("Total number of timeout responses"), { domain_l }).set_skip_when_empty(),
                sm::make_counter("delay_samples", std::bind(&domain::count_all, this, &stats::delay_samples),
                        sm::description("Total number of delay samples"), { domain_l }),
                sm::make_counter("delay_total", [this] () -> double {
                            std::chrono::duration<double> res(0);
                            for (const auto& m : list) {
                                res += m._c._stats.delay_total;
                            }
                            return res.count();
                        }, sm::description("Total delay in seconds"), { domain_l }),
                sm::make_gauge("pending", std::bind(&domain::count_all_fn, this, &client::outgoing_queue_length),
                    sm::description("Number of queued outbound messages"), { domain_l }),
                sm::make_gauge("wait_reply", std::bind(&domain::count_all_fn, this, &client::incoming_queue_length),
                    sm::description("Number of replies waiting for"), { domain_l }),
          });
      }
  };

  thread_local std::unordered_map<sstring, client::metrics::domain> client::metrics::domain::all;

  client::metrics::domain& client::metrics::domain::find_or_create(sstring name) {
      auto i = all.try_emplace(name, name);
      return i.first->second;
  }

  client::metrics::metrics(const client& c)
        : _c(c)
        , _domain(domain::find_or_create(_c._options.metrics_domain))
  {
      _domain.list.push_back(*this);
  }

  client::metrics::~metrics() {
      _domain.dead.replied += _c._stats.replied;
      _domain.dead.exception_received += _c._stats.exception_received;
      _domain.dead.sent_messages += _c._stats.sent_messages;
      _domain.dead.timeout += _c._stats.timeout;
      _domain.dead.delay_samples += _c._stats.delay_samples;
      _domain.dead.delay_total += _c._stats.delay_total;
  }

  client::client(const logger& l, void* s, client_options ops, socket socket, const socket_address& addr, const socket_address& local)
  : rpc::connection(l, s), _socket(std::move(socket)), _server_addr(addr), _local_addr(local), _options(ops), _metrics(*this)
  {
       _socket.set_reuseaddr(ops.reuseaddr);
      // Run client in the background.
      // Communicate result via _stopped.
      // The caller has to call client::stop() to synchronize.
      (void)_socket.connect(addr, local).then([this, ops = std::move(ops)] (connected_socket fd) {
          fd.set_nodelay(ops.tcp_nodelay);
          if (ops.keepalive) {
              fd.set_keepalive(true);
              fd.set_keepalive_parameters(ops.keepalive.value());
          }
          set_socket(std::move(fd));

          feature_map features;
          if (_options.compressor_factory) {
              features[protocol_features::COMPRESS] = _options.compressor_factory->supported();
          }
          if (_options.send_timeout_data) {
              features[protocol_features::TIMEOUT] = "";
          }
          if (_options.send_handler_duration) {
              features[protocol_features::HANDLER_DURATION] = "";
          }
          if (_options.stream_parent) {
              features[protocol_features::STREAM_PARENT] = serialize_connection_id(_options.stream_parent);
          }
          if (!_options.isolation_cookie.empty()) {
              features[protocol_features::ISOLATION] = _options.isolation_cookie;
          }

          return negotiate_protocol(std::move(features)).then([this] {
              _propagate_timeout = !is_stream();
              set_negotiated();
              return do_until([this] { return _read_buf.eof() || _error; }, [this] () mutable {
                  if (is_stream()) {
                      return handle_stream_frame();
                  }
                  return read_response_frame_compressed(_read_buf).then([this] (response_frame::return_type msg_id_and_data) {
                      auto& msg_id = std::get<0>(msg_id_and_data);
                      auto& data = std::get<2>(msg_id_and_data);
                      auto it = _outstanding.find(std::abs(msg_id));
                      if (!data) {
                          _error = true;
                      } else if (it != _outstanding.end()) {
                          auto handler = std::move(it->second);
                          auto ht = std::get<1>(msg_id_and_data);
                          _outstanding.erase(it);
                          (*handler)(*this, msg_id, std::move(data.value()));
                          if (ht) {
                              _stats.delay_samples++;
                              _stats.delay_total += (rpc_clock_type::now() - handler->start) - std::chrono::microseconds(*ht);
                          }
                      } else if (msg_id < 0) {
                          try {
                              std::rethrow_exception(unmarshal_exception(data.value()));
                          } catch(const unknown_verb_error& ex) {
                              // if this is unknown verb exception with unknown id ignore it
                              // can happen if unknown verb was used by no_wait client
                              get_logger()(peer_address(), format("unknown verb exception {:d} ignored", ex.type));
                          } catch(...) {
                              // We've got error response but handler is no longer waiting, could be timed out.
                              log_exception(*this, log_level::info, "ignoring error response", std::current_exception());
                          }
                      } else {
                          // we get a reply for a message id not in _outstanding
                          // this can happened if the message id is timed out already
                          get_logger()(peer_address(), log_level::debug, "got a reply for an expired message id");
                      }
                  });
              });
          });
      }).then_wrapped([this] (future<> f) {
          std::exception_ptr ep;
          if (f.failed()) {
              ep = f.get_exception();
              if (_connected) {
                  if (is_stream()) {
                      log_exception(*this, log_level::error, "client stream connection dropped", ep);
                  } else {
                      log_exception(*this, log_level::error, "client connection dropped", ep);
                  }
              } else {
                  if (is_stream()) {
                      log_exception(*this, log_level::debug, "stream fail to connect", ep);
                  } else {
                      log_exception(*this, log_level::debug, "fail to connect", ep);
                  }
              }
          }
          if (is_stream() && (ep || _error)) {
              _stream_queue.abort(std::make_exception_ptr(stream_closed()));
          }
          _error = true;
          return stop_send_loop(ep).then_wrapped([this] (future<> f) {
              f.ignore_ready_future();
              _outstanding.clear();
              if (is_stream()) {
                  deregister_this_stream();
              } else {
                  abort_all_streams();
              }
          }).finally([this] {
              return _compressor ? _compressor->close() : make_ready_future();
          }).finally([this]{
              _stopped.set_value();
          });
      });
      enqueue_zero_frame();
  }

  client::client(const logger& l, void* s, const socket_address& addr, const socket_address& local)
  : client(l, s, client_options{}, make_socket(), addr, local)
  {}

  client::client(const logger& l, void* s, client_options options, const socket_address& addr, const socket_address& local)
  : client(l, s, options, make_socket(), addr, local)
  {}

  client::client(const logger& l, void* s, socket socket, const socket_address& addr, const socket_address& local)
  : client(l, s, client_options{}, std::move(socket), addr, local)
  {}


  future<feature_map>
  server::connection::negotiate(feature_map requested) {
      feature_map ret;
      future<> f = make_ready_future<>();
      for (auto&& e : requested) {
          auto id = e.first;
          switch (id) {
          // supported features go here
          case protocol_features::COMPRESS: {
              if (get_server()._options.compressor_factory) {
                  _compressor = get_server()._options.compressor_factory->negotiate(e.second, true, [this] { return send({}); });
                  if (_compressor) {
                       ret[protocol_features::COMPRESS] = _compressor->name();
                  }
              }
          }
          break;
          case protocol_features::TIMEOUT:
              _timeout_negotiated = true;
              ret[protocol_features::TIMEOUT] = "";
              break;
          case protocol_features::HANDLER_DURATION:
              _handler_duration_negotiated = true;
              ret[protocol_features::HANDLER_DURATION] = "";
              break;
          case protocol_features::STREAM_PARENT: {
              if (!get_server()._options.streaming_domain) {
                  f = f.then([] {
                      return make_exception_future<>(std::runtime_error("streaming is not configured for the server"));
                  });
              } else {
                  _parent_id = deserialize_connection_id(e.second);
                  _is_stream = true;
                  // remove stream connection from rpc connection list
                  get_server()._conns.erase(get_connection_id());
                  f = f.then([this, c = shared_from_this()] () mutable {
                      return smp::submit_to(_parent_id.shard(), [this, c = make_foreign(static_pointer_cast<rpc::connection>(c))] () mutable {
                          auto sit = _servers.find(*get_server()._options.streaming_domain);
                          if (sit == _servers.end()) {
                              throw std::logic_error(format("Shard {:d} does not have server with streaming domain {}", this_shard_id(), *get_server()._options.streaming_domain).c_str());
                          }
                          auto s = sit->second;
                          auto it = s->_conns.find(_parent_id);
                          if (it == s->_conns.end()) {
                              throw std::logic_error(format("Unknown parent connection {} on shard {:d}", _parent_id, this_shard_id()).c_str());
                          }
                          if (it->second->_error) {
                              throw std::runtime_error(format("Parent connection {} is aborting on shard {:d}", _parent_id, this_shard_id()).c_str());
                          }
                          auto id = c->get_connection_id();
                          it->second->register_stream(id, make_lw_shared(std::move(c)));
                      });
                  });
              }
              break;
          }
          case protocol_features::ISOLATION: {
              auto&& isolation_cookie = e.second;
              struct isolation_function_visitor {
                  isolation_function_visitor(const sstring& isolation_cookie) :
                        _isolation_cookie(isolation_cookie) { }
                  future<isolation_config> operator() (resource_limits::syncronous_isolation_function f) const {
                      return futurize_invoke(f, _isolation_cookie);
                  }
                  future<isolation_config> operator() (resource_limits::asyncronous_isolation_function f) const {
                      return f(_isolation_cookie);
                  }
              private:
                  sstring _isolation_cookie;
              };

              auto visitor = isolation_function_visitor(isolation_cookie);
              f = f.then([visitor = std::move(visitor), this] () mutable {
                  return std::visit(visitor, get_server()._limits.isolate_connection).then([this] (isolation_config conf) {
                      _isolation_config = conf;
                  });
              });
              ret.emplace(e);
              break;
          }
          default:
              // nothing to do
              ;
          }
      }
      if (get_server()._options.streaming_domain) {
          ret[protocol_features::CONNECTION_ID] = serialize_connection_id(_id);
      }
      return f.then([ret = std::move(ret)] {
          return ret;
      });
  }

  future<>
  server::connection::negotiate_protocol() {
      return receive_negotiation_frame(*this, _read_buf).then([this] (feature_map requested_features) {
          return negotiate(std::move(requested_features)).then([this] (feature_map returned_features) {
              return send_negotiation_frame(std::move(returned_features));
          });
      });
  }

  future<request_frame::return_type>
  server::connection::read_request_frame_compressed(input_stream<char>& in) {
      if (_timeout_negotiated) {
          return read_frame_compressed<request_frame_with_timeout>(_info.addr, _compressor, in);
      } else {
          return read_frame_compressed<request_frame>(_info.addr, _compressor, in);
      }
  }

  future<>
  server::connection::respond(int64_t msg_id, snd_buf&& data, std::optional<rpc_clock_type::time_point> timeout, std::optional<rpc_clock_type::duration> handler_duration) {
      if (_handler_duration_negotiated) {
          response_frame_with_handler_time::encode_header(msg_id, handler_duration, data);
      } else {
          data.front().trim_front(sizeof(uint32_t));
          data.size -= sizeof(uint32_t);
          response_frame::encode_header(msg_id, data);
      }
      return send(std::move(data), timeout);
  }

future<> server::connection::send_unknown_verb_reply(std::optional<rpc_clock_type::time_point> timeout, int64_t msg_id, uint64_t type) {
    return wait_for_resources(28, timeout).then([this, timeout, msg_id, type] (auto permit) {
        // send unknown_verb exception back
        constexpr size_t unknown_verb_message_size = response_frame_headroom + 2 * sizeof(uint32_t) + sizeof(uint64_t);
        snd_buf data(unknown_verb_message_size);
        static_assert(snd_buf::chunk_size >= unknown_verb_message_size, "send buffer chunk size is too small");
        auto p = data.front().get_write() + response_frame_headroom;
        write_le<uint32_t>(p, uint32_t(exception_type::UNKNOWN_VERB));
        write_le<uint32_t>(p + 4, uint32_t(8));
        write_le<uint64_t>(p + 8, type);
        try {
            // Send asynchronously.
            // This is safe since connection::stop() will wait for background work.
            (void)with_gate(get_server()._reply_gate, [this, timeout, msg_id, data = std::move(data), permit = std::move(permit)] () mutable {
                // workaround for https://gcc.gnu.org/bugzilla/show_bug.cgi?id=83268
                auto c = shared_from_this();
                return respond(-msg_id, std::move(data), timeout, std::nullopt).then([c = std::move(c), permit = std::move(permit)] {});
            });
        } catch(gate_closed_exception&) {/* ignore */}
    });
}

  future<> server::connection::process() {
      return negotiate_protocol().then([this] () mutable {
        auto sg = _isolation_config ? _isolation_config->sched_group : current_scheduling_group();
        return with_scheduling_group(sg, [this] {
          set_negotiated();
          return do_until([this] { return _read_buf.eof() || _error; }, [this] () mutable {
              if (is_stream()) {
                  return handle_stream_frame();
              }
              return read_request_frame_compressed(_read_buf).then([this] (request_frame::return_type header_and_buffer) {
                  auto& expire = std::get<0>(header_and_buffer);
                  auto& type = std::get<1>(header_and_buffer);
                  auto& msg_id = std::get<2>(header_and_buffer);
                  auto& data = std::get<3>(header_and_buffer);
                  if (!data) {
                      _error = true;
                      return make_ready_future<>();
                  } else {
                      std::optional<rpc_clock_type::time_point> timeout;
                      if (expire && *expire) {
                          timeout = relative_timeout_to_absolute(std::chrono::milliseconds(*expire));
                      }
                      auto h = get_server()._proto.get_handler(type);
                      if (!h) {
                          return send_unknown_verb_reply(timeout, msg_id, type);
                      }

                      // If the new method of per-connection scheduling group was used, honor it.
                      // Otherwise, use the old per-handler scheduling group.
                      auto sg = _isolation_config ? _isolation_config->sched_group : h->handler.sg;
                      return with_scheduling_group(sg, [this, timeout, msg_id, &h = h->handler, data = std::move(data.value()), guard = std::move(h->holder)] () mutable {
                          return h.func(shared_from_this(), timeout, msg_id, std::move(data), std::move(guard));
                      });
                  }
              });
          });
        });
      }).then_wrapped([this] (future<> f) {
          std::exception_ptr ep;
          if (f.failed()) {
              ep = f.get_exception();
              log_exception(*this, log_level::error,
                      format("server{} connection dropped", is_stream() ? " stream" : "").c_str(), ep);
          }
          _fd.shutdown_input();
          if (is_stream() && (ep || _error)) {
              _stream_queue.abort(std::make_exception_ptr(stream_closed()));
          }
          _error = true;
          return stop_send_loop(ep).then_wrapped([this] (future<> f) {
              f.ignore_ready_future();
              get_server()._conns.erase(get_connection_id());
              if (is_stream()) {
                  return deregister_this_stream();
              } else {
                  return abort_all_streams();
              }
          }).finally([this] {
              return _compressor ? _compressor->close() : make_ready_future();
          }).finally([this] {
              _stopped.set_value();
          });
      }).finally([conn_ptr = shared_from_this()] {
          // hold onto connection pointer until do_until() exists
      });
  }

  server::connection::connection(server& s, connected_socket&& fd, socket_address&& addr, const logger& l, void* serializer, connection_id id)
          : rpc::connection(std::move(fd), l, serializer, id)
          , _info{.addr{std::move(addr)}, .server{s}, .conn_id{id}} {
  }

  future<> server::connection::deregister_this_stream() {
      if (!get_server()._options.streaming_domain) {
          return make_ready_future<>();
      }
      return smp::submit_to(_parent_id.shard(), [this] () mutable {
          auto sit = server::_servers.find(*get_server()._options.streaming_domain);
          if (sit != server::_servers.end()) {
              auto s = sit->second;
              auto it = s->_conns.find(_parent_id);
              if (it != s->_conns.end()) {
                  it->second->_streams.erase(get_connection_id());
              }
          }
      });
  }

  future<> server::connection::abort_all_streams() {
      return parallel_for_each(_streams | boost::adaptors::map_values, [] (xshard_connection_ptr s) {
          return smp::submit_to(s->get_owner_shard(), [s] {
              s->get()->abort();
          });
      }).then([this] {
          _streams.clear();
      });
  }

  thread_local std::unordered_map<streaming_domain_type, server*> server::_servers;

  server::server(protocol_base* proto, const socket_address& addr, resource_limits limits)
      : server(proto, seastar::listen(addr, listen_options{true}), limits, server_options{})
  {}

  server::server(protocol_base* proto, server_options opts, const socket_address& addr, resource_limits limits)
      : server(proto, seastar::listen(addr, listen_options{true, opts.load_balancing_algorithm}), limits, opts)
  {}

  server::server(protocol_base* proto, server_socket ss, resource_limits limits, server_options opts)
          : _proto(*proto), _ss(std::move(ss)), _limits(limits), _resources_available(limits.max_memory), _options(opts)
  {
      if (_options.streaming_domain) {
          if (_servers.find(*_options.streaming_domain) != _servers.end()) {
              throw std::runtime_error(format("An RPC server with the streaming domain {} is already exist", *_options.streaming_domain));
          }
          _servers[*_options.streaming_domain] = this;
      }
      accept();
  }

  server::server(protocol_base* proto, server_options opts, server_socket ss, resource_limits limits)
          : server(proto, std::move(ss), limits, opts)
  {}

  void server::accept() {
      // Run asynchronously in background.
      // Communicate result via __ss_stopped.
      // The caller has to call server::stop() to synchronize.
      (void)keep_doing([this] () mutable {
          return _ss.accept().then([this] (accept_result ar) mutable {
              if (_options.filter_connection && !_options.filter_connection(ar.remote_address)) {
                  return;
              }
              auto fd = std::move(ar.connection);
              auto addr = std::move(ar.remote_address);
              fd.set_nodelay(_options.tcp_nodelay);
              connection_id id = _options.streaming_domain ?
                      connection_id::make_id(_next_client_id++, uint16_t(this_shard_id())) :
                      connection_id::make_invalid_id(_next_client_id++);
              auto conn = _proto.make_server_connection(*this, std::move(fd), std::move(addr), id);
              auto r = _conns.emplace(id, conn);
              SEASTAR_ASSERT(r.second);
              // Process asynchronously in background.
              (void)conn->process();
          });
      }).then_wrapped([this] (future<>&& f){
          try {
              f.get();
              SEASTAR_ASSERT(false);
          } catch (...) {
              _ss_stopped.set_value();
          }
      });
  }

  future<> server::shutdown() {
      if (_shutdown) {
          return make_ready_future<>();
      }

      _ss.abort_accept();
      _resources_available.broken();
      if (_options.streaming_domain) {
          _servers.erase(*_options.streaming_domain);
      }
      return _ss_stopped.get_future().then([this] {
          return parallel_for_each(_conns | boost::adaptors::map_values, [] (shared_ptr<connection> conn) {
              return conn->stop();
          });
      }).finally([this] {
          _shutdown = true;
      });
  }

  future<> server::stop() {
      return when_all(
          shutdown(),
          _reply_gate.close()
      ).discard_result();
  }

  void server::abort_connection(connection_id id) {
      auto it = _conns.find(id);
      if (it == _conns.end()) {
          return;
      }
      try {
          it->second->abort();
      } catch (...) {
          log_exception(*it->second, log_level::error,
                        "fail to shutdown connection on user request", std::current_exception());
      }
  }

  std::ostream& operator<<(std::ostream& os, const connection_id& id) {
      fmt::print(os, "{:x}", id.id());
      return os;
  }

  std::ostream& operator<<(std::ostream& os, const streaming_domain_type& domain) {
      fmt::print(os, "{:d}", domain._id);
      return os;
  }

  isolation_config default_isolate_connection(sstring isolation_cookie) {
      return isolation_config{};
  }

multi_algo_compressor_factory::multi_algo_compressor_factory(std::vector<const rpc::compressor::factory*> factories)
        : _factories(std::move(factories)) {
    _features =  boost::algorithm::join(_factories | boost::adaptors::transformed(std::mem_fn(&rpc::compressor::factory::supported)), sstring(","));
}

std::unique_ptr<compressor>
multi_algo_compressor_factory::negotiate(sstring feature, bool is_server, std::function<future<>()> send_empty_frame) const {
    std::vector<sstring> names;
    boost::split(names, feature, boost::is_any_of(","));
    std::unique_ptr<compressor> c;
    if (is_server) {
        for (auto&& n : names) {
            for (auto&& f : _factories) {
                if ((c = f->negotiate(n, is_server, send_empty_frame))) {
                    return c;
                }
            }
        }
    } else {
        for (auto&& f : _factories) {
            for (auto&& n : names) {
                if ((c = f->negotiate(n, is_server, send_empty_frame))) {
                    return c;
                }
            }
        }
    }
    return nullptr;
}

}

}
