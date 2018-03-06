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

  void
  client::negotiate(feature_map provided) {
      // record features returned here
      for (auto&& e : provided) {
          auto id = e.first;
          switch (id) {
          // supported features go here
          case protocol_features::COMPRESS:
              if (_options.compressor_factory) {
                  this->_compressor = _options.compressor_factory->negotiate(e.second, false);
              }
              break;
          case protocol_features::TIMEOUT:
              this->_timeout_negotiated = true;
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
      return this->template read_frame<response_frame>(this->_server_addr, in);
  }

  future<int64_t, std::experimental::optional<rcv_buf>>
  client::read_response_frame_compressed(input_stream<char>& in) {
      return this->template read_frame_compressed<response_frame>(this->_server_addr, this->_compressor, in);
  }

  stats client::get_stats() const {
      stats res = this->_stats;
      res.wait_reply = _outstanding.size();
      res.pending = this->_outgoing_queue.size();
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
      this->_stats.timeout++;
      _outstanding[id]->timeout();
      _outstanding.erase(id);
  }

  future<> client::stop() {
      if (!this->_error) {
          this->_error = true;
          _socket.shutdown();
      }
      return this->_stopped.get_future();
  }

  client::client(const logger& l, void* s, client_options ops, socket socket, ipv4_addr addr, ipv4_addr local)
  : rpc::connection(l, s), _socket(std::move(socket)), _server_addr(addr), _options(ops) {
      _socket.connect(addr, local).then([this, ops = std::move(ops)] (connected_socket fd) {
          fd.set_nodelay(ops.tcp_nodelay);
          if (ops.keepalive) {
              fd.set_keepalive(true);
              fd.set_keepalive_parameters(ops.keepalive.value());
          }
          this->set_socket(std::move(fd));

          feature_map features;
          if (_options.compressor_factory) {
              features[protocol_features::COMPRESS] = _options.compressor_factory->supported();
          }
          if (_options.send_timeout_data) {
              features[protocol_features::TIMEOUT] = "";
          }
          send_negotiation_frame(std::move(features));

          return this->negotiate_protocol(this->_read_buf).then([this] () {
              send_loop();
              return do_until([this] { return this->_read_buf.eof() || this->_error; }, [this] () mutable {
                  return this->read_response_frame_compressed(this->_read_buf).then([this] (int64_t msg_id, std::experimental::optional<rcv_buf> data) {
                      auto it = _outstanding.find(std::abs(msg_id));
                      if (!data) {
                          this->_error = true;
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
                              this->get_logger()(this->peer_address(), sprint("unknown verb exception %d ignored", ex.type));
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
              log_exception(*this, this->_connected ? "client connection dropped" : "fail to connect", f.get_exception());
          }
          this->_error = true;
          this->stop_send_loop().then_wrapped([this] (future<> f) {
              f.ignore_ready_future();
              this->_stopped.set_value();
              this->_outstanding.clear();
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


}

}
