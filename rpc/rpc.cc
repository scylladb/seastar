#include "rpc.hh"

namespace seastar {

namespace rpc {
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

  temporary_buffer<char>& snd_buf::front() {
      auto *one = boost::get<temporary_buffer<char>>(&bufs);
      if (one) {
          return *one;
      } else {
          return boost::get<std::vector<temporary_buffer<char>>>(bufs).front();
      }
  }

  snd_buf connection::compress(snd_buf buf) {
      if (_compressor) {
          buf = _compressor->compress(4, std::move(buf));
          static_assert(snd_buf::chunk_size >= 4, "send buffer chunk size is too small");
          write_le<uint32_t>(buf.front().get_write(), buf.size - 4);
          return std::move(buf);
      }
      return std::move(buf);
  }

  future<> connection::send_buffer(snd_buf buf) {
      auto* b = boost::get<temporary_buffer<char>>(&buf.bufs);
      if (b) {
          return _write_buf.write(std::move(*b));
      } else {
          return do_with(std::move(boost::get<std::vector<temporary_buffer<char>>>(buf.bufs)),
                  [this] (std::vector<temporary_buffer<char>>& ar) {
              return do_for_each(ar.begin(), ar.end(), [this] (auto& b) {
                  return _write_buf.write(std::move(b));
              });
          });
      }
  }

  template<connection::outgoing_queue_type QueueType>
  void connection::send_loop() {
      _send_loop_stopped = do_until([this] { return _error; }, [this] {
          return _outgoing_queue_cond.wait([this] { return !_outgoing_queue.empty(); }).then([this] {
              // despite using wait with predicated above _outgoing_queue can still be empty here if
              // there is only one entry on the list and its expire timer runs after wait() returned ready future,
              // but before this continuation runs.
              if (_outgoing_queue.empty()) {
                  return make_ready_future();
              }
              auto d = std::move(_outgoing_queue.front());
              _outgoing_queue.pop_front();
              d.t.cancel(); // cancel timeout timer
              if (d.pcancel) {
                  d.pcancel->cancel_send = std::function<void()>(); // request is no longer cancellable
              }
              if (QueueType == outgoing_queue_type::request) {
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
              d.buf = compress(std::move(d.buf));
              auto f = send_buffer(std::move(d.buf)).then([this] {
                  _stats.sent_messages++;
                  return _write_buf.flush();
              });
              return f.finally([d = std::move(d)] {});
          });
      }).handle_exception([this] (std::exception_ptr eptr) {
          _error = true;
      });
  }
  void connection::send_loop(connection::outgoing_queue_type type) {
      if (type == outgoing_queue_type::request) {
          send_loop<outgoing_queue_type::request>();
      } else {
          send_loop<outgoing_queue_type::response>();
      }
  }

  future<> connection::stop_send_loop() {
      _error = true;
      if (_connected) {
          _outgoing_queue_cond.broken();
          _fd.shutdown_output();
      }
      return _send_loop_stopped.finally([this] {
          _outgoing_queue.clear();
          return _connected ? _write_buf.close() : make_ready_future();
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

  future<> connection::send(snd_buf buf, std::experimental::optional<rpc_clock_type::time_point> timeout, cancellable* cancel) {
      if (!_error) {
          if (timeout && *timeout <= rpc_clock_type::now()) {
              return make_ready_future<>();
          }
          _outgoing_queue.emplace_back(std::move(buf));
          auto deleter = [this, it = std::prev(_outgoing_queue.cend())] {
              _outgoing_queue.erase(it);
          };
          if (timeout) {
              auto& t = _outgoing_queue.back().t;
              t.set_callback(deleter);
              t.arm(timeout.value());
          }
          if (cancel) {
              cancel->cancel_send = std::move(deleter);
              cancel->send_back_pointer = &_outgoing_queue.back().pcancel;
              _outgoing_queue.back().pcancel = cancel;
          }
          _outgoing_queue_cond.signal();
          return _outgoing_queue.back().p->get_future();
      } else {
          return make_exception_future<>(closed_error());
      }
  }

  future<> connection::stop() {
      if (!_error) {
          _error = true;
          _fd.shutdown_input();
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
              c.get_logger()(c.peer_address(), "wrong protocol magic");
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
                              boost::get<std::vector<temporary_buffer<char>>>(rb.bufs).push_back(std::move(data));
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

  template<typename FrameType, typename Info>
  typename FrameType::return_type
  connection::read_frame(const Info& info, input_stream<char>& in) {
      auto header_size = FrameType::header_size();
      return in.read_exactly(header_size).then([this, header_size, &info, &in] (temporary_buffer<char> header) {
          if (header.size() != header_size) {
              if (header.size() != 0) {
                  _logger(info, sprint("unexpected eof on a %s while reading header: expected %d got %d", FrameType::role(), header_size, header.size()));
              }
              return FrameType::empty_value();
          }
          auto h = FrameType::decode_header(header.get());
          auto size = FrameType::get_size(h);
          if (!size) {
              return FrameType::make_value(h, rcv_buf());
          } else {
              return read_rcv_buf(in, size).then([this, &info, h = std::move(h), size] (rcv_buf rb) {
                  if (rb.size != size) {
                      _logger(info, sprint("unexpected eof on a %s while reading data: expected %d got %d", FrameType::role(), size, rb.size));
                      return FrameType::empty_value();
                  } else {
                      return FrameType::make_value(h, std::move(rb));
                  }
              });
          }
      });
  }

  template<typename FrameType, typename Info>
  typename FrameType::return_type
  connection::read_frame_compressed(const Info& info, std::unique_ptr<compressor>& compressor, input_stream<char>& in) {
      if (compressor) {
          return in.read_exactly(4).then([&] (temporary_buffer<char> compress_header) {
              if (compress_header.size() != 4) {
                  if (compress_header.size() != 0) {
                      _logger(info, sprint("unexpected eof on a %s while reading compression header: expected 4 got %d", FrameType::role(), compress_header.size()));
                  }
                  return FrameType::empty_value();
              }
              auto ptr = compress_header.get();
              auto size = read_le<uint32_t>(ptr);
              return read_rcv_buf(in, size).then([this, size, &compressor, &info] (rcv_buf compressed_data) {
                  if (compressed_data.size != size) {
                      _logger(info, sprint("unexpected eof on a %s while reading compressed data: expected %d got %d", FrameType::role(), size, compressed_data.size));
                      return FrameType::empty_value();
                  }
                  auto eb = compressor->decompress(std::move(compressed_data));
                  net::packet p;
                  auto* one = boost::get<temporary_buffer<char>>(&eb.bufs);
                  if (one) {
                      p = net::packet(std::move(p), std::move(*one));
                  } else {
                      for (auto&& b : boost::get<std::vector<temporary_buffer<char>>>(eb.bufs)) {
                          p = net::packet(std::move(p), std::move(b));
                      }
                  }
                  return do_with(as_input_stream(std::move(p)), [this, &info] (input_stream<char>& in) {
                      return read_frame<FrameType>(info, in);
                  });
              });
          });
      } else {
          return read_frame<FrameType>(info, in);
      }
  }

  template<typename Connection>
  static void log_exception(Connection& c, const char* log, std::exception_ptr eptr) {
      const char* s;
      try {
          std::rethrow_exception(eptr);
      } catch (std::exception& ex) {
          s = ex.what();
      } catch (...) {
          s = "unknown exception";
      }
      c.get_logger()(c.peer_address(), sprint("%s: %s", log, s));
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
              break;
          case protocol_features::TIMEOUT:
              _timeout_negotiated = true;
              break;
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
      using opt_buf_type = std::experimental::optional<rcv_buf>;
      using return_type = future<int64_t, opt_buf_type>;
      using header_type = std::tuple<int64_t, uint32_t>;
      static size_t header_size() {
          return 12;
      }
      static const char* role() {
          return "client";
      }
      static auto empty_value() {
          return make_ready_future<int64_t, opt_buf_type>(0, std::experimental::nullopt);
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
          return make_ready_future<int64_t, opt_buf_type>(std::get<0>(t), std::move(data));
      }
  };


  future<int64_t, std::experimental::optional<rcv_buf>>
  client::read_response_frame(input_stream<char>& in) {
      return read_frame<response_frame>(_server_addr, in);
  }

  future<int64_t, std::experimental::optional<rcv_buf>>
  client::read_response_frame_compressed(input_stream<char>& in) {
      return read_frame_compressed<response_frame>(_server_addr, _compressor, in);
  }

  stats client::get_stats() const {
      stats res = _stats;
      res.wait_reply = _outstanding.size();
      res.pending = _outgoing_queue.size();
      return res;
  }

  void client::wait_for_reply(id_type id, std::unique_ptr<reply_handler_base>&& h, std::experimental::optional<rpc_clock_type::time_point> timeout, cancellable* cancel) {
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

  future<> client::stop() {
      if (!_error) {
          _error = true;
          _socket.shutdown();
      }
      return _stopped.get_future();
  }

  client::client(const logger& l, void* s, client_options ops, socket socket, ipv4_addr addr, ipv4_addr local)
  : rpc::connection(l, s), _socket(std::move(socket)), _server_addr(addr), _options(ops) {
      _socket.connect(addr, local).then([this, ops = std::move(ops)] (connected_socket fd) {
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
          send_negotiation_frame(std::move(features));

          return negotiate_protocol(_read_buf).then([this] () {
              send_loop();
              return do_until([this] { return _read_buf.eof() || _error; }, [this] () mutable {
                  return read_response_frame_compressed(_read_buf).then([this] (int64_t msg_id, std::experimental::optional<rcv_buf> data) {
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
                              get_logger()(peer_address(), sprint("unknown verb exception %d ignored", ex.type));
                          } catch(...) {
                              // We've got error response but handler is no longer waiting, could be timed out.
                              log_exception(*this, "ignoring error response", std::current_exception());
                          }
                      } else {
                          // we get a reply for a message id not in _outstanding
                          // this can happened if the message id is timed out already
                          // FIXME: log it but with low level, currently log levels are not supported
                      }
                  });
              });
          });
      }).then_wrapped([this] (future<> f) {
          if (f.failed()) {
              log_exception(*this, _connected ? "client connection dropped" : "fail to connect", f.get_exception());
          }
          _error = true;
          stop_send_loop().then_wrapped([this] (future<> f) {
              f.ignore_ready_future();
              _stopped.set_value();
              _outstanding.clear();
          });
      });
  }

  client::client(const logger& l, void* s, ipv4_addr addr, ipv4_addr local)
  : client(l, s, client_options{}, engine().net().socket(), addr, local)
  {}

  client::client(const logger& l, void* s, client_options options, ipv4_addr addr, ipv4_addr local)
  : client(l, s, options, engine().net().socket(), addr, local)
  {}

  client::client(const logger& l, void* s, socket socket, ipv4_addr addr, ipv4_addr local)
  : client(l, s, client_options{}, std::move(socket), addr, local)
  {}


  feature_map
  server::connection::negotiate(feature_map requested) {
      feature_map ret;
      for (auto&& e : requested) {
          auto id = e.first;
          switch (id) {
          // supported features go here
          case protocol_features::COMPRESS: {
              if (_server._options.compressor_factory) {
                  _compressor = _server._options.compressor_factory->negotiate(e.second, true);
                  ret[protocol_features::COMPRESS] = _server._options.compressor_factory->supported();
              }
          }
          break;
          case protocol_features::TIMEOUT:
              _timeout_negotiated = true;
              ret[protocol_features::TIMEOUT] = "";
              break;
          default:
              // nothing to do
              ;
          }
      }
      return ret;
  }

  future<>
  server::connection::negotiate_protocol(input_stream<char>& in) {
      return receive_negotiation_frame(*this, in).then([this] (feature_map requested_features) {
          auto returned_features = negotiate(std::move(requested_features));
          return send_negotiation_frame(std::move(returned_features));
      });
  }

  struct request_frame {
      using opt_buf_type = std::experimental::optional<rcv_buf>;
      using return_type = future<std::experimental::optional<uint64_t>, uint64_t, int64_t, opt_buf_type>;
      using header_type = std::tuple<std::experimental::optional<uint64_t>, uint64_t, int64_t, uint32_t>;
      static size_t header_size() {
          return 20;
      }
      static const char* role() {
          return "server";
      }
      static auto empty_value() {
          return make_ready_future<std::experimental::optional<uint64_t>, uint64_t, int64_t, opt_buf_type>(std::experimental::nullopt, uint64_t(0), 0, std::experimental::nullopt);
      }
      static header_type decode_header(const char* ptr) {
          auto type = read_le<uint64_t>(ptr);
          auto msgid = read_le<int64_t>(ptr + 8);
          auto size = read_le<uint32_t>(ptr + 16);
          return std::make_tuple(std::experimental::nullopt, type, msgid, size);
      }
      static uint32_t get_size(const header_type& t) {
          return std::get<3>(t);
      }
      static auto make_value(const header_type& t, rcv_buf data) {
          return make_ready_future<std::experimental::optional<uint64_t>, uint64_t, int64_t, opt_buf_type>(std::get<0>(t), std::get<1>(t), std::get<2>(t), std::move(data));
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

  future<std::experimental::optional<uint64_t>, uint64_t, int64_t, std::experimental::optional<rcv_buf>>
  server::connection::read_request_frame_compressed(input_stream<char>& in) {
      if (_timeout_negotiated) {
          return read_frame_compressed<request_frame_with_timeout>(_info, _compressor, in);
      } else {
          return read_frame_compressed<request_frame>(_info, _compressor, in);
      }
  }

  future<>
  server::connection::respond(int64_t msg_id, snd_buf&& data, std::experimental::optional<rpc_clock_type::time_point> timeout) {
      static_assert(snd_buf::chunk_size >= 12, "send buffer chunk size is too small");
      auto p = data.front().get_write();
      write_le<int64_t>(p, msg_id);
      write_le<uint32_t>(p + 8, data.size - 12);
      return send(std::move(data), timeout);
  }

  future<> server::connection::process() {
      return negotiate_protocol(_read_buf).then([this] () mutable {
          send_loop();
          return do_until([this] { return _read_buf.eof() || _error; }, [this] () mutable {
              return read_request_frame_compressed(_read_buf).then([this] (std::experimental::optional<uint64_t> expire, uint64_t type, int64_t msg_id, std::experimental::optional<rcv_buf> data) {
                  if (!data) {
                      _error = true;
                      return make_ready_future<>();
                  } else {
                      std::experimental::optional<rpc_clock_type::time_point> timeout;
                      if (expire && *expire) {
                          timeout = rpc_clock_type::now() + std::chrono::milliseconds(*expire);
                      }
                      auto h = _server._proto->get_handler(type);
                      if (h) {
                          return (*h)(shared_from_this(), timeout, msg_id, std::move(data.value()));
                      } else {
                          return wait_for_resources(28, timeout).then([this, timeout, msg_id, type] (auto permit) {
                              // send unknown_verb exception back
                              snd_buf data(28);
                              static_assert(snd_buf::chunk_size >= 28, "send buffer chunk size is too small");
                              auto p = data.front().get_write() + 12;
                              write_le<uint32_t>(p, uint32_t(exception_type::UNKNOWN_VERB));
                              write_le<uint32_t>(p + 4, uint32_t(8));
                              write_le<uint64_t>(p + 8, type);
                              try {
                                  with_gate(_server._reply_gate, [this, timeout, msg_id, data = std::move(data), permit = std::move(permit)] () mutable {
                                      // workaround for https://gcc.gnu.org/bugzilla/show_bug.cgi?id=83268
                                      auto c = shared_from_this();
                                      return respond(-msg_id, std::move(data), timeout).then([c = std::move(c), permit = std::move(permit)] {});
                                  });
                              } catch(gate_closed_exception&) {/* ignore */}
                          });
                      }
                  }
              });
          });
      }).then_wrapped([this] (future<> f) {
          if (f.failed()) {
              log_exception(*this, "server connection dropped", f.get_exception());
          }
          _error = true;
          return stop_send_loop().then_wrapped([this] (future<> f) {
              f.ignore_ready_future();
              _server._conns.erase(shared_from_this());
              _stopped.set_value();
          });
      }).finally([conn_ptr = shared_from_this()] {
          // hold onto connection pointer until do_until() exists
      });
  }

  server::connection::connection(server& s, connected_socket&& fd, socket_address&& addr, const logger& l, void* serializer)
      : rpc::connection(std::move(fd), l, serializer), _server(s) {
      _info.addr = std::move(addr);
  }

  server::server(protocol_base* proto, ipv4_addr addr, resource_limits limits)
      : server(proto, engine().listen(addr, listen_options(true)), limits, server_options{})
  {}

  server::server(protocol_base* proto, server_options opts, ipv4_addr addr, resource_limits limits)
      : server(proto, engine().listen(addr, listen_options(true)), limits, opts)
  {}

  server::server(protocol_base* proto, server_socket ss, resource_limits limits, server_options opts)
          : _proto(proto), _ss(std::move(ss)), _limits(limits), _resources_available(limits.max_memory), _options(opts)
  {
      accept();
  }

  server::server(protocol_base* proto, server_options opts, server_socket ss, resource_limits limits)
          : server(proto, std::move(ss), limits, opts)
  {}

  void server::accept() {
      keep_doing([this] () mutable {
          return _ss.accept().then([this] (connected_socket fd, socket_address addr) mutable {
              fd.set_nodelay(_options.tcp_nodelay);
              auto conn = _proto->make_server_connection(*this, std::move(fd), std::move(addr));
              _conns.insert(conn);
              conn->process();
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
      return when_all(_ss_stopped.get_future(),
          parallel_for_each(_conns, [] (lw_shared_ptr<connection> conn) {
              return conn->stop();
          }),
          _reply_gate.close()
      ).discard_result();
  }

}

}
