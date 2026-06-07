#pragma once
#include "io_uring.h"
#include "non_owning_co_future.h"
namespace HTTP {

class ReadIterator {
  IOUring &ring_;
  std::array<char, kReadBufferSize> buffer_;
  size_t length_{0};
  size_t position_{0};
  int fd_;
  bool eof_{false};

public:
  ReadIterator(IOUring &ring, int fd_);
  NonOwningCoFuture<void> operator++();
  char operator*() const;
  explicit operator bool() const;
  bool Eof() const;
  size_t Available() const;
  const char *CurrentPtr() const;
  void Advance(size_t n);
};
} // namespace HTTP
