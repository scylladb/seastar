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
}

}
