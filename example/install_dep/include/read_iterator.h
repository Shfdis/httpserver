#pragma once
#include "coroutine.h"
#include "io_uring.h"
#include "request_data.h"
namespace HTTP {
class ReadIterator {
  IOUring &ring_;
  std::array<char, 256> buffer_;
  size_t length_{0};
  size_t position_{0};
  int fd_;

public:
  ReadIterator(IOUring &ring, int fd_);
  Coroutine Ensure();
  size_t Available() const;
  const char *CurrentPtr() const;
  void Advance(size_t n);
  Coroutine operator++();
  char operator*();
  operator bool();
  Coroutine ParseVariables(RequestData &data);
  Coroutine ParseHeaders(RequestData &data);
  Coroutine ParseMethod(RequestData &data);
  Coroutine ParseBody(RequestData &data);
};
}; // namespace HTTP
