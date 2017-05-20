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
}

}
