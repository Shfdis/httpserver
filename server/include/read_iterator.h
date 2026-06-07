#pragma once
#include "co_future.h"
#include "io_uring.h"
namespace HTTP {

class ReadIterator {
  IOUring &ring_;
  std::array<char, 256> buffer_;
  size_t length_{0};
  size_t position_{0};
  int fd_;
  bool eof_{false};

public:
  ReadIterator(IOUring &ring, int fd_);
  CoFuture<void> operator++();
  char operator*() const;
  explicit operator bool() const;
  bool Eof() const;
  size_t Available() const;
  const char *CurrentPtr() const;
  void Advance(size_t n);
};
} // namespace HTTP
