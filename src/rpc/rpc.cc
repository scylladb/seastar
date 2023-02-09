#include <seastar/rpc/rpc.hh>
#include <seastar/core/align.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/print.hh>
#include <seastar/core/future-util.hh>
#include <boost/range/adaptor/map.hpp>

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

  constexpr size_t snd_buf::chunk_size;

  snd_buf::snd_buf(size_t size_) : size(size_) {
      if (size <= chunk_size) {
          bufs = temporary_buffer<char>(size);
      } else {
          std::vector<temporary_buffer<char>> v;
          v.reserve(align_up(size_t(size), chunk_size) / chunk_size);
          while (size_) {
              v.push_back(temporary_buffer<char>(std::min(chunk_size, size_)));
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
              newbufs.push_back(temporary_buffer<char>(b.get_write(), b.size(), d.share()));
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

  future<> connection::send_entry(outgoing_entry& d) {
      if (_propagate_timeout) {
          static_assert(snd_buf::chunk_size >= 8, "send buffer chunk size is too small");
          if (_timeout_negotiated) {
              auto expire = d.t.get_timeout();
              uint64_t left = 0;
              if (expire != typename timer<rpc_clock_type>::time_point()) {
                  left = std::chrono::duration_cast<std::chrono::milliseconds>(expire - timer<rpc_clock_type>::clock::now()).count();
              }
              write_le<uint64_t>(d.buf.front().get_write(), left);
          } else {
              d.buf.front().trim_front(8);
              d.buf.size -= 8;
          }
      }
      auto buf = compress(std::move(d.buf));
      return send_buffer(std::move(buf)).then([this] {
          _stats.sent_messages++;
          return _write_buf.flush();
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
          bool sink_closed = std::get<1>(res).get0();
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
      assert(it != _outgoing_queue.end());

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
              // Front entry is most likely (unless _negotiated is unresolved) sitting
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
                  _error |= f.failed();
                  f.ignore_ready_future();
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
  typename FrameType::return_type
  connection::read_frame(socket_address info, input_stream<char>& in) {
      auto header_size = FrameType::header_size();
      return in.read_exactly(header_size).then([this, header_size, info, &in] (temporary_buffer<char> header) {
          if (header.size() != header_size) {
              if (header.size() != 0) {
                  _logger(info, format("unexpected eof on a {} while reading header: expected {:d} got {:d}", FrameType::role(), header_size, header.size()));
              }
              return FrameType::empty_value();
          }
          auto h = FrameType::decode_header(header.get());
          auto size = FrameType::get_size(h);
          if (!size) {
              return FrameType::make_value(h, rcv_buf());
          } else {
              return read_rcv_buf(in, size).then([this, info, h = std::move(h), size] (rcv_buf rb) {
                  if (rb.size != size) {
                      _logger(info, format("unexpected eof on a {} while reading data: expected {:d} got {:d}", FrameType::role(), size, rb.size));
                      return FrameType::empty_value();
                  } else {
                      return FrameType::make_value(h, std::move(rb));
                  }
              });
          }
      });
  }

  template<typename FrameType>
  typename FrameType::return_type
  connection::read_frame_compressed(socket_address info, std::unique_ptr<compressor>& compressor, input_stream<char>& in) {
      if (compressor) {
          return in.read_exactly(4).then([this, info, &in, &compressor] (temporary_buffer<char> compress_header) {
              if (compress_header.size() != 4) {
                  if (compress_header.size() != 0) {
                      _logger(info, format("unexpected eof on a {} while reading compression header: expected 4 got {:d}", FrameType::role(), compress_header.size()));
                  }
                  return FrameType::empty_value();
              }
              auto ptr = compress_header.get();
              auto size = read_le<uint32_t>(ptr);
              return read_rcv_buf(in, size).then([this, size, &compressor, info] (rcv_buf compressed_data) {
                  if (compressed_data.size != size) {
                      _logger(info, format("unexpected eof on a {} while reading compressed data: expected {:d} got {:d}", FrameType::role(), size, compressed_data.size));
                      return FrameType::empty_value();
                  }
                  auto eb = compressor->decompress(std::move(compressed_data));
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
      using return_type = future<opt_buf_type>;
      struct header_type {
          uint32_t size;
          bool eos;
      };
      static size_t header_size() {
          return 4;
      }
      static const char* role() {
          return "stream";
      }
      static future<opt_buf_type> empty_value() {
          return make_ready_future<opt_buf_type>(std::nullopt);
      }
      static header_type decode_header(const char* ptr) {
          header_type h{read_le<uint32_t>(ptr), false};
          if (h.size == -1U) {
              h.size = 0;
              h.eos = true;
          }
          return h;
      }
      static uint32_t get_size(const header_type& t) {
          return t.size;
      }
      static future<opt_buf_type> make_value(const header_type& t, rcv_buf data) {
          if (t.eos) {
              data.size = -1U;
          }
          return make_ready_future<opt_buf_type>(std::move(data));
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
              assert(_stream_queue.empty());
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

  void
  client::negotiate(feature_map provided) {
      // record features returned here
      for (auto&& e : provided) {
          auto id = e.first;
          switch (id) {
          // supported features go here
          case protocol_features::COMPRESS:
              if (_options.compressor_factory) {
                  _compressor = _options.compressor_factory->negotiate(e.second, false);
              }
              if (!_compressor) {
                  throw std::runtime_error(format("RPC server responded with compression {} - unsupported", e.second));
              }
              break;
          case protocol_features::TIMEOUT:
              _timeout_negotiated = true;
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

  future<>
  client::negotiate_protocol(input_stream<char>& in) {
      return receive_negotiation_frame(*this, in).then([this] (feature_map features) {
          return negotiate(features);
      });
  }

  struct response_frame {
      using opt_buf_type = std::optional<rcv_buf>;
      using header_and_buffer_type = std::tuple<int64_t, opt_buf_type>;
      using return_type = future<header_and_buffer_type>;
      using header_type = std::tuple<int64_t, uint32_t>;
      static size_t header_size() {
          return 12;
      }
      static const char* role() {
          return "client";
      }
      static auto empty_value() {
          return make_ready_future<header_and_buffer_type>(header_and_buffer_type(0, std::nullopt));
      }
      static header_type decode_header(const char* ptr) {
          auto msgid = read_le<int64_t>(ptr);
          auto size = read_le<uint32_t>(ptr + 8);
          return std::make_tuple(msgid, size);
      }
      static uint32_t get_size(const header_type& t) {
          return std::get<1>(t);
      }
      static auto make_value(const header_type& t, rcv_buf data) {
          return make_ready_future<header_and_buffer_type>(header_and_buffer_type(std::get<0>(t), std::move(data)));
      }
  };


  future<response_frame::header_and_buffer_type>
  client::read_response_frame(input_stream<char>& in) {
      return read_frame<response_frame>(_server_addr, in);
  }

  future<response_frame::header_and_buffer_type>
  client::read_response_frame_compressed(input_stream<char>& in) {
      return read_frame_compressed<response_frame>(_server_addr, _compressor, in);
  }

  stats client::get_stats() const {
      stats res = _stats;
      res.wait_reply = _outstanding.size();
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
          assert(s->second->get_owner_shard() == this_shard_id()); // abort can be called only locally
          s->second->get()->abort();
          _streams.erase(s);
      }
  }

  void client::deregister_this_stream() {
      if (_parent) {
          _parent->_streams.erase(_id);
      }
  }

  client::client(const logger& l, void* s, client_options ops, socket socket, const socket_address& addr, const socket_address& local)
  : rpc::connection(l, s), _socket(std::move(socket)), _server_addr(addr), _local_addr(local), _options(ops) {
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
          if (_options.stream_parent) {
              features[protocol_features::STREAM_PARENT] = serialize_connection_id(_options.stream_parent);
          }
          if (!_options.isolation_cookie.empty()) {
              features[protocol_features::ISOLATION] = _options.isolation_cookie;
          }

          return send_negotiation_frame(std::move(features)).then([this] {
               return negotiate_protocol(_read_buf);
          }).then([this] () {
              _propagate_timeout = !is_stream();
              set_negotiated();
              return do_until([this] { return _read_buf.eof() || _error; }, [this] () mutable {
                  if (is_stream()) {
                      return handle_stream_frame();
                  }
                  return read_response_frame_compressed(_read_buf).then([this] (std::tuple<int64_t, std::optional<rcv_buf>> msg_id_and_data) {
                      auto& msg_id = std::get<0>(msg_id_and_data);
                      auto& data = std::get<1>(msg_id_and_data);
                      auto it = _outstanding.find(std::abs(msg_id));
                      if (!data) {
                          _error = true;
                      } else if (it != _outstanding.end()) {
                          auto handler = std::move(it->second);
                          _outstanding.erase(it);
                          (*handler)(*this, msg_id, std::move(data.value()));
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
          _error = true;
          _stream_queue.abort(std::make_exception_ptr(stream_closed()));
          return stop_send_loop(ep).then_wrapped([this] (future<> f) {
              f.ignore_ready_future();
              _outstanding.clear();
              if (is_stream()) {
                  deregister_this_stream();
              } else {
                  abort_all_streams();
              }
          }).finally([this]{
              _stopped.set_value();
          });
      });
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
              if (_server._options.compressor_factory) {
                  _compressor = _server._options.compressor_factory->negotiate(e.second, true);
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
          case protocol_features::STREAM_PARENT: {
              if (!_server._options.streaming_domain) {
                  f = f.then([] {
                      return make_exception_future<>(std::runtime_error("streaming is not configured for the server"));
                  });
              } else {
                  _parent_id = deserialize_connection_id(e.second);
                  _is_stream = true;
                  // remove stream connection from rpc connection list
                  _server._conns.erase(get_connection_id());
                  f = f.then([this, c = shared_from_this()] () mutable {
                      return smp::submit_to(_parent_id.shard(), [this, c = make_foreign(static_pointer_cast<rpc::connection>(c))] () mutable {
                          auto sit = _servers.find(*_server._options.streaming_domain);
                          if (sit == _servers.end()) {
                              throw std::logic_error(format("Shard {:d} does not have server with streaming domain {}", this_shard_id(), *_server._options.streaming_domain).c_str());
                          }
                          auto s = sit->second;
                          auto it = s->_conns.find(_parent_id);
                          if (it == s->_conns.end()) {
                              throw std::logic_error(format("Unknown parent connection {} on shard {:d}", _parent_id, this_shard_id()).c_str());
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
                  return std::visit(visitor, _server._limits.isolate_connection).then([this] (isolation_config conf) {
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
      if (_server._options.streaming_domain) {
          ret[protocol_features::CONNECTION_ID] = serialize_connection_id(_id);
      }
      return f.then([ret = std::move(ret)] {
          return ret;
      });
  }

  future<>
  server::connection::negotiate_protocol(input_stream<char>& in) {
      return receive_negotiation_frame(*this, in).then([this] (feature_map requested_features) {
          return negotiate(std::move(requested_features)).then([this] (feature_map returned_features) {
              return send_negotiation_frame(std::move(returned_features));
          });
      });
  }

  struct request_frame {
      using opt_buf_type = std::optional<rcv_buf>;
      using header_and_buffer_type = std::tuple<std::optional<uint64_t>, uint64_t, int64_t, opt_buf_type>;
      using return_type = future<header_and_buffer_type>;
      using header_type = std::tuple<std::optional<uint64_t>, uint64_t, int64_t, uint32_t>;
      static size_t header_size() {
          return 20;
      }
      static const char* role() {
          return "server";
      }
      static auto empty_value() {
          return make_ready_future<header_and_buffer_type>(header_and_buffer_type(std::nullopt, uint64_t(0), 0, std::nullopt));
      }
      static header_type decode_header(const char* ptr) {
          auto type = read_le<uint64_t>(ptr);
          auto msgid = read_le<int64_t>(ptr + 8);
          auto size = read_le<uint32_t>(ptr + 16);
          return std::make_tuple(std::nullopt, type, msgid, size);
      }
      static uint32_t get_size(const header_type& t) {
          return std::get<3>(t);
      }
      static auto make_value(const header_type& t, rcv_buf data) {
          return make_ready_future<header_and_buffer_type>(header_and_buffer_type(std::get<0>(t), std::get<1>(t), std::get<2>(t), std::move(data)));
      }
  };

  struct request_frame_with_timeout : request_frame {
      using super = request_frame;
      static size_t header_size() {
          return 28;
      }
      static typename super::header_type decode_header(const char* ptr) {
          auto h = super::decode_header(ptr + 8);
          std::get<0>(h) = read_le<uint64_t>(ptr);
          return h;
      }
  };

  future<request_frame::header_and_buffer_type>
  server::connection::read_request_frame_compressed(input_stream<char>& in) {
      if (_timeout_negotiated) {
          return read_frame_compressed<request_frame_with_timeout>(_info.addr, _compressor, in);
      } else {
          return read_frame_compressed<request_frame>(_info.addr, _compressor, in);
      }
  }

  future<>
  server::connection::respond(int64_t msg_id, snd_buf&& data, std::optional<rpc_clock_type::time_point> timeout) {
      static_assert(snd_buf::chunk_size >= 12, "send buffer chunk size is too small");
      auto p = data.front().get_write();
      write_le<int64_t>(p, msg_id);
      write_le<uint32_t>(p + 8, data.size - 12);
      return send(std::move(data), timeout);
  }

future<> server::connection::send_unknown_verb_reply(std::optional<rpc_clock_type::time_point> timeout, int64_t msg_id, uint64_t type) {
    return wait_for_resources(28, timeout).then([this, timeout, msg_id, type] (auto permit) {
        // send unknown_verb exception back
        snd_buf data(28);
        static_assert(snd_buf::chunk_size >= 28, "send buffer chunk size is too small");
        auto p = data.front().get_write() + 12;
        write_le<uint32_t>(p, uint32_t(exception_type::UNKNOWN_VERB));
        write_le<uint32_t>(p + 4, uint32_t(8));
        write_le<uint64_t>(p + 8, type);
        try {
            // Send asynchronously.
            // This is safe since connection::stop() will wait for background work.
            (void)with_gate(_server._reply_gate, [this, timeout, msg_id, data = std::move(data), permit = std::move(permit)] () mutable {
                // workaround for https://gcc.gnu.org/bugzilla/show_bug.cgi?id=83268
                auto c = shared_from_this();
                return respond(-msg_id, std::move(data), timeout).then([c = std::move(c), permit = std::move(permit)] {});
            });
        } catch(gate_closed_exception&) {/* ignore */}
    });
}

  future<> server::connection::process() {
      return negotiate_protocol(_read_buf).then([this] () mutable {
        auto sg = _isolation_config ? _isolation_config->sched_group : current_scheduling_group();
        return with_scheduling_group(sg, [this] {
          set_negotiated();
          return do_until([this] { return _read_buf.eof() || _error; }, [this] () mutable {
              if (is_stream()) {
                  return handle_stream_frame();
              }
              return read_request_frame_compressed(_read_buf).then([this] (request_frame::header_and_buffer_type header_and_buffer) {
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
                      auto h = _server._proto->get_handler(type);
                      if (!h) {
                          return send_unknown_verb_reply(timeout, msg_id, type);
                      }

                      // If the new method of per-connection scheduling group was used, honor it.
                      // Otherwise, use the old per-handler scheduling group.
                      auto sg = _isolation_config ? _isolation_config->sched_group : h->sg;
                      return with_scheduling_group(sg, [this, timeout, msg_id, h, data = std::move(data.value())] () mutable {
                          return h->func(shared_from_this(), timeout, msg_id, std::move(data)).finally([this, h] {
                              // If anything between get_handler() and here throws, we leak put_handler
                              _server._proto->put_handler(h);
                          });
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
          _error = true;
          _stream_queue.abort(std::make_exception_ptr(stream_closed()));
          return stop_send_loop(ep).then_wrapped([this] (future<> f) {
              f.ignore_ready_future();
              _server._conns.erase(get_connection_id());
              if (is_stream()) {
                  return deregister_this_stream();
              } else {
                  return make_ready_future<>();
              }
          }).finally([this] {
              _stopped.set_value();
          });
      }).finally([conn_ptr = shared_from_this()] {
          // hold onto connection pointer until do_until() exists
      });
  }

  server::connection::connection(server& s, connected_socket&& fd, socket_address&& addr, const logger& l, void* serializer, connection_id id)
      : rpc::connection(std::move(fd), l, serializer, id), _server(s) {
      _info.addr = std::move(addr);
  }

  future<> server::connection::deregister_this_stream() {
      if (!_server._options.streaming_domain) {
          return make_ready_future<>();
      }
      return smp::submit_to(_parent_id.shard(), [this] () mutable {
          auto sit = server::_servers.find(*_server._options.streaming_domain);
          if (sit != server::_servers.end()) {
              auto s = sit->second;
              auto it = s->_conns.find(_parent_id);
              if (it != s->_conns.end()) {
                  it->second->_streams.erase(get_connection_id());
              }
          }
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
          : _proto(proto), _ss(std::move(ss)), _limits(limits), _resources_available(limits.max_memory), _options(opts)
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
              auto conn = _proto->make_server_connection(*this, std::move(fd), std::move(addr), id);
              auto r = _conns.emplace(id, conn);
              assert(r.second);
              // Process asynchronously in background.
              (void)conn->process();
          });
      }).then_wrapped([this] (future<>&& f){
          try {
              f.get();
              assert(false);
          } catch (...) {
              _ss_stopped.set_value();
          }
      });
  }

  future<> server::stop() {
      _ss.abort_accept();
      _resources_available.broken();
      if (_options.streaming_domain) {
          _servers.erase(*_options.streaming_domain);
      }
      return when_all(_ss_stopped.get_future(),
          parallel_for_each(_conns | boost::adaptors::map_values, [] (shared_ptr<connection> conn) {
              return conn->stop();
          }),
          _reply_gate.close()
      ).discard_result();
  }

  std::ostream& operator<<(std::ostream& os, const connection_id& id) {
      fmt::print(os, "{:x}", id.id);
      return os;
  }

  std::ostream& operator<<(std::ostream& os, const streaming_domain_type& domain) {
      fmt::print(os, "{:d}", domain._id);
      return os;
  }

  isolation_config default_isolate_connection(sstring isolation_cookie) {
      return isolation_config{};
  }

}

}
