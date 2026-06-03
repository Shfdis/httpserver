#pragma once
#include "co_future.h"
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
  CoFuture<void> Ensure();
  size_t Available() const;
  const char *CurrentPtr() const;
  void Advance(size_t n);
  CoFuture<void> operator++();
  char operator*();
  operator bool();
  CoFuture<void> ParseVariables(RequestData &data);
  CoFuture<void> ParseHeaders(RequestData &data);
  CoFuture<void> ParseMethod(RequestData &data);
  CoFuture<void> ParseBody(RequestData &data);
};
};
